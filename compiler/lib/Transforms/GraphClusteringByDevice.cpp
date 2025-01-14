//===- GraphClusteringByDevice.cpp ----------------------------*--- C++ -*-===//
//
// Copyright 2022 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//

#include "byteir/Transforms/GraphClusteringByDevice.h"

#include "byteir/Dialect/mhlo/Util/Util.h"
#include "byteir/Utils/IRRewrite.h"
#include "byteir/Utils/Utils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Operation.h"
#include "llvm/ADT/SetVector.h"

#include <list>
#include <numeric>

#include "PassDetail.h"

using namespace mlir;
using namespace llvm;

namespace {

constexpr const char *DEVICE_ATTR_HOST = "host";

struct FunctionMetadata {
  StringRef anchorName;
  // The device where function will run
  StringRef deviceAttr;
  // The original function name before partition.
  StringRef originalName;
  // The insertion point of partition functions.
  Block::iterator insertionPoint;
  // The partitioned function name.
  llvm::StringRef partitionName;
  // The input values of the function.
  llvm::SmallVector<Value, 4> inputs;
  // The result values of the function.
  llvm::SmallVector<Value, 4> results;
  // The operations to be included in the body of the function.
  llvm::SmallVector<Operation *, 8> ops;

  func::FuncOp partitionOp;
};

void insertOpsRecursively(Operation *op, SmallDenseSet<Operation *> &opSet) {
  auto pair = opSet.insert(op);
  if (!pair.second)
    return;
  for (Value v : op->getOperands()) {
    if (Operation *defOp = v.getDefiningOp()) {
      insertOpsRecursively(defOp, opSet);
    }
  }
}

bool isHostOp(Operation &op, StringRef attrName) {
  for (auto &region : op.getRegions()) {
    for (auto &block : region.getBlocks()) {
      for (auto &innerOp : block.getOperations()) {
        if (isHostOp(innerOp, attrName))
          return true;
      }
    }
  }

  if (op.hasAttr(attrName)) {
    StringAttr attr = op.getAttrOfType<StringAttr>(attrName);
    if (attr.getValue().str() == DEVICE_ATTR_HOST) {
      return true;
    }
  }
  return false;
}

std::optional<SmallVector<FunctionMetadata, 4>>
getFunctionMetadatasFallback(func::FuncOp funcOp, StringRef attrName,
                             StringRef deviceAttr, StringRef deviceAnchorName,
                             bool dupOutputs,
                             ValidateSubGraphFn validateSubGraphFn) {
  SmallVector<FunctionMetadata, 4> metadatas;
  SmallDenseSet<Operation *> hostOps;
  for (Operation &op : funcOp.front().without_terminator()) {
    if (isHostOp(op, attrName)) {
      insertOpsRecursively(&op, hostOps);
    }
  }

  Operation &retOp = funcOp.front().back();
  llvm::DenseMap<Value, int64_t> retStats;
  for (const auto &operand : retOp.getOperands()) {
    if (retStats.count(operand)) {
      retStats[operand] += 1;
    } else {
      retStats.insert(std::make_pair(operand, 1));
    }
  }

  if (hostOps.size() > 0) {
    FunctionMetadata hostFuncMetadata;
    hostFuncMetadata.anchorName = getHostAnchorName();
    hostFuncMetadata.deviceAttr = DEVICE_ATTR_HOST;
    hostFuncMetadata.originalName = funcOp.getSymName();
    hostFuncMetadata.insertionPoint = ++Block::iterator(funcOp);
    for (Operation &op : funcOp.front().without_terminator()) {
      if (hostOps.count(&op)) {
        hostFuncMetadata.ops.push_back(&op);
      }
    }
    hostFuncMetadata.inputs = getInputsOfCluster(hostFuncMetadata.ops);
    hostFuncMetadata.results = getOutputsOfCluster(
        hostFuncMetadata.ops, dupOutputs ? &retStats : nullptr);
    metadatas.push_back(hostFuncMetadata);
  }

  FunctionMetadata deviceFuncMetadata;
  deviceFuncMetadata.anchorName = deviceAnchorName;
  deviceFuncMetadata.deviceAttr = deviceAttr;
  deviceFuncMetadata.originalName = funcOp.getSymName();
  deviceFuncMetadata.insertionPoint = ++Block::iterator(funcOp);
  for (Operation &op : funcOp.front().without_terminator()) {
    if (!hostOps.count(&op)) {
      deviceFuncMetadata.ops.push_back(&op);
    }
  }
  if (deviceFuncMetadata.ops.size() > 0) {
    if (validateSubGraphFn != nullptr &&
        !validateSubGraphFn(deviceFuncMetadata.ops)) {
      return std::nullopt;
    }
    deviceFuncMetadata.inputs = getInputsOfCluster(deviceFuncMetadata.ops);
    deviceFuncMetadata.results = getOutputsOfCluster(
        deviceFuncMetadata.ops, dupOutputs ? &retStats : nullptr);

    metadatas.push_back(deviceFuncMetadata);
  }

  return metadatas;
}

struct ActiveDeviceCluster {
  using OpList = llvm::SetVector<Operation *, std::vector<Operation *>>;
  using OpClusterMap = llvm::DenseMap<Operation *, ActiveDeviceCluster>;
  OpList operations;
  ActiveDeviceCluster *mergedInto;

  ActiveDeviceCluster(Operation *op) {
    operations.insert(op);
    mergedInto = nullptr;
  }

  ActiveDeviceCluster *getRoot() {
    if (!this->mergedInto)
      return this;

    return this->mergedInto = this->mergedInto->getRoot();
  }

  bool isBeforeInBlock(ActiveDeviceCluster *other) {
    if (this->operations.back()->isBeforeInBlock(other->operations.front())) {
      return true;
    }
    return false;
  }

  // return merged ActiveDeviceCluster or nullptr for merge failure
  // arg order sensitive, prefer merge lhs into rhs
  static ActiveDeviceCluster *tryMerge(ActiveDeviceCluster *lhs,
                                       ActiveDeviceCluster *rhs,
                                       OpClusterMap &op2cluster);

  struct CompareByNumOps {
    bool operator()(ActiveDeviceCluster *lhs, ActiveDeviceCluster *rhs) {
      return lhs->operations.size() > rhs->operations.size();
    }
  };

private:
  static bool tryMergeInto(ActiveDeviceCluster *from, ActiveDeviceCluster *to,
                           OpClusterMap &op2cluster);

  static bool anyDefIn(Operation *op, const OpList &operations) {
    for (auto &region : op->getRegions()) {
      for (auto &block : region.getBlocks()) {
        for (auto &innerOp : block.getOperations()) {
          if (anyDefIn(&innerOp, operations)) {
            return true;
          }
        }
      }
    }

    for (auto &&operand : op->getOperands())
      if (operations.count(operand.getDefiningOp()))
        return true;
    return false;
  }

  static bool anyUseIn(Operation *op, const OpList &operations) {
    for (auto &&use : op->getUses()) {
      auto *owner = use.getOwner();
      for (auto *op : operations)
        if (op->isAncestor(owner))
          return true;

      if (operations.count(owner))
        return true;
    }
    return false;
  }

  // operations in \p src that can be moved up to \p target will be store in
  // \p moveUp in pre-order, and the remaining operations will be kept in \p src
  // in pre-order
  static auto computeMoveUpSet(const OpList &target, OpList &src,
                               OpList &moveUp, OpClusterMap &op2cluster) {
    std::vector<Operation *> vec = src.takeVector();
    OpList &remain = src;
    for (auto &&op : vec) {
      if (remain.contains(op))
        continue;
      if (anyDefIn(op, target) || anyDefIn(op, remain)) {
        auto &&iter = op2cluster.find(op);
        OpList ops;
        if (iter == op2cluster.end()) {
          remain.insert(op);
          continue;
        }
        ActiveDeviceCluster *cluster = iter->second.getRoot();
        for (Operation *clusterOp : cluster->operations) {
          assert(std::find(vec.begin(), vec.end(), clusterOp) != vec.end());
          assert(remain.insert(clusterOp));
          if (moveUp.contains(clusterOp)) {
            moveUp.remove(clusterOp);
          }
        }
      } else {
        moveUp.insert(op);
      }
    }
  }

  // operations in \p src that can be moved down to \p target will be store in
  // \p moveDown in post-order, and the remaining operations will be kept in \p
  // src in pre-order
  static auto computeMoveDownSet(const OpList &target, OpList &src,
                                 OpList &moveDown, OpClusterMap &op2cluster) {
    std::vector<Operation *> vec = src.takeVector();
    OpList &remain = src;
    for (auto &&op : llvm::reverse(vec)) {
      if (remain.contains(op))
        continue;
      if (anyUseIn(op, target) || anyUseIn(op, remain)) {
        auto &&iter = op2cluster.find(op);
        OpList ops;
        if (iter == op2cluster.end()) {
          remain.insert(op);
          continue;
        }
        ActiveDeviceCluster *cluster = iter->second.getRoot();
        for (Operation *clusterOp : llvm::reverse(cluster->operations)) {
          assert(std::find(vec.begin(), vec.end(), clusterOp) != vec.end());
          assert(remain.insert(clusterOp));
          if (moveDown.contains(clusterOp)) {
            moveDown.remove(clusterOp);
          }
        }
      } else {
        moveDown.insert(op);
      }
    }
    vec = remain.takeVector();
    remain.insert(vec.rbegin(), vec.rend());
  }
};

bool ActiveDeviceCluster::tryMergeInto(ActiveDeviceCluster *from,
                                       ActiveDeviceCluster *to,
                                       OpClusterMap &op2cluster) {
  static auto takePointer = [](Operation &op) { return &op; };
  if (from->isBeforeInBlock(to)) {
    OpList toMove(
        llvm::map_iterator(std::next(from->operations.back()->getIterator()),
                           takePointer),
        llvm::map_iterator(to->operations.front()->getIterator(), takePointer));
    OpList moveUp, moveDown;

    computeMoveUpSet(from->operations, toMove, moveUp, op2cluster);
    computeMoveDownSet(to->operations, toMove, moveDown, op2cluster);

    if (!toMove.empty())
      return false;

    for (auto &&op : moveUp) {
      op->moveBefore(from->operations.front());
    }

    for (auto &&op : moveDown) {
      op->moveAfter(to->operations.back());
    }

    std::vector<Operation *> toOperations = to->operations.takeVector();
    from->operations.insert(toOperations.begin(), toOperations.end());
    to->operations = std::move(from->operations);
  } else {
    assert(to->isBeforeInBlock(from) && "invalid cluster order");
    OpList toMove(
        llvm::map_iterator(std::next(to->operations.back()->getIterator()),
                           takePointer),
        llvm::map_iterator(from->operations.front()->getIterator(),
                           takePointer));
    OpList moveUp, moveDown;

    computeMoveDownSet(from->operations, toMove, moveDown, op2cluster);
    computeMoveUpSet(to->operations, toMove, moveUp, op2cluster);

    if (!toMove.empty())
      return false;

    for (auto &&op : moveUp) {
      op->moveBefore(to->operations.front());
    }

    for (auto &&op : moveDown) {
      op->moveAfter(from->operations.back());
    }

    std::vector<Operation *> fromOperations = from->operations.takeVector();
    to->operations.insert(fromOperations.begin(), fromOperations.end());
  }

  from->mergedInto = to;
  return true;
}

ActiveDeviceCluster *ActiveDeviceCluster::tryMerge(ActiveDeviceCluster *lhs,
                                                   ActiveDeviceCluster *rhs,
                                                   OpClusterMap &op2cluster) {
  if (!lhs || !rhs || lhs == rhs)
    return nullptr;

  if (lhs->mergedInto || rhs->mergedInto)
    return nullptr;

  if (tryMergeInto(lhs, rhs, op2cluster)) {
    return rhs;
  }

  if (tryMergeInto(rhs, lhs, op2cluster)) {
    return lhs;
  }

  return nullptr;
}

class DeviceClusteringAlgoBaseHelper {
public:
  std::optional<SmallVector<FunctionMetadata, 4>>
  getFunctionMetadatas(StringRef attrName, StringRef deviceAttr,
                       StringRef deviceAnchorName, bool dupOutputs,
                       bool enableMultiGraph,
                       ValidateSubGraphFn validateSubGraphFn);

protected:
  DeviceClusteringAlgoBaseHelper(func::FuncOp funcOp, StringRef attrName);

  ActiveDeviceCluster *getCluster(Operation *op) {
    auto &&iter = op2cluster.find(op);
    if (iter == op2cluster.end())
      return nullptr;

    return iter->second.getRoot();
  }

  ActiveDeviceCluster *getCluster(Value value) {
    return getCluster(value.getDefiningOp());
  }

  // void mergeDeviceClustersProgressively();
  void populateCandidates();

  func::FuncOp funcOp;
  llvm::DenseMap<Operation *, ActiveDeviceCluster> op2cluster;
  std::vector<ActiveDeviceCluster *> candidates;
};

DeviceClusteringAlgoBaseHelper::DeviceClusteringAlgoBaseHelper(
    func::FuncOp funcOp, StringRef attrName)
    : funcOp(funcOp) {
  for (auto &&op : funcOp.front().without_terminator()) {
    if (isHostOp(op, attrName)) {
      continue;
    }
    // if a constant is only used by host op, mark it as host
    if (isMhloConstantLike(&op) && op.getResult(0).hasOneUse()) {
      Operation *user = *op.getResult(0).getUsers().begin();
      if (isHostOp(*user, attrName)) {
        continue;
      }
    }
    op2cluster.try_emplace(&op, ActiveDeviceCluster(&op));
  }
}

std::optional<SmallVector<FunctionMetadata, 4>>
DeviceClusteringAlgoBaseHelper::getFunctionMetadatas(
    StringRef attrName, StringRef deviceAttr, StringRef deviceAnchorName,
    bool dupOutputs, bool enableMultiGraph,
    ValidateSubGraphFn validateSubGraphFn) {
  if (candidates.empty())
    return std::nullopt;

  auto &&firstCluster = candidates[0];
  if (firstCluster->operations.empty())
    return std::nullopt;

  SmallVector<FunctionMetadata, 4> metadatas;
  Operation &retOp = funcOp.front().back();
  llvm::DenseMap<Value, int64_t> retStats;
  for (const auto &operand : retOp.getOperands()) {
    if (retStats.count(operand)) {
      retStats[operand] += 1;
    } else {
      retStats.insert(std::make_pair(operand, 1));
    }
  }

  for (auto cluster : candidates) {
    if (cluster->operations.empty())
      continue;
    if (validateSubGraphFn != nullptr &&
        !validateSubGraphFn(cluster->operations.getArrayRef()))
      continue;
    FunctionMetadata deviceFuncMetadata;
    deviceFuncMetadata.anchorName = deviceAnchorName;
    deviceFuncMetadata.deviceAttr = deviceAttr;
    deviceFuncMetadata.originalName = funcOp.getSymName();
    deviceFuncMetadata.insertionPoint = ++Block::iterator(funcOp);
    deviceFuncMetadata.ops = llvm::to_vector(cluster->operations);
    deviceFuncMetadata.inputs = getInputsOfCluster(deviceFuncMetadata.ops);
    deviceFuncMetadata.results = getOutputsOfCluster(
        deviceFuncMetadata.ops, dupOutputs ? &retStats : nullptr);
    metadatas.push_back(deviceFuncMetadata);
    if (!enableMultiGraph)
      break;
  }

  return metadatas;
}

void DeviceClusteringAlgoBaseHelper::populateCandidates() {
  std::list<ActiveDeviceCluster *> workList;
  for (auto &&[_, cluster] : op2cluster) {
    if (!cluster.mergedInto) {
      workList.push_back(&cluster);
    }
  }
  workList.sort(ActiveDeviceCluster::CompareByNumOps());
  candidates.clear();
  while (!workList.empty()) {
    ActiveDeviceCluster *cluster = workList.front();
    workList.pop_front();
    for (auto &&iter = workList.begin(); iter != workList.end();) {
      if (auto merged =
              ActiveDeviceCluster::tryMerge(*iter, cluster, op2cluster)) {
        cluster = merged;
        iter = workList.erase(iter);
      } else {
        iter++;
      }
    }
    candidates.push_back(cluster);
  }
  llvm::sort(candidates, ActiveDeviceCluster::CompareByNumOps());
}

// all derived classes are expected to implement
// `mergeDeviceClustersProgressively()`
template <typename Derived>
class DeviceClusteringAlgoBase : public DeviceClusteringAlgoBaseHelper {
public:
  DeviceClusteringAlgoBase(func::FuncOp funcOp, StringRef attrName)
      : DeviceClusteringAlgoBaseHelper(funcOp, attrName) {
    static_assert(std::is_base_of<DeviceClusteringAlgoBase, Derived>::value);

    static_cast<Derived *>(this)->mergeDeviceClustersProgressively();
    populateCandidates();
  }
};

class TopDownDeviceClustering
    : public DeviceClusteringAlgoBase<TopDownDeviceClustering> {
public:
  using DeviceClusteringAlgoBase::DeviceClusteringAlgoBase;
  void mergeDeviceClustersProgressively();
};

void TopDownDeviceClustering::mergeDeviceClustersProgressively() {
  SmallVector<Operation *> ops;
  for (auto &op : funcOp.front().without_terminator())
    ops.push_back(&op);

  for (auto op : ops) {
    auto curCluster = getCluster(op);
    for (auto &&operand : op->getOperands()) {
      auto preCluster = getCluster(operand);
      if (auto merged = ActiveDeviceCluster::tryMerge(preCluster, curCluster,
                                                      op2cluster)) {
        curCluster = merged;
      }
    }
  }
}

class BottomUpDeviceClustering
    : public DeviceClusteringAlgoBase<BottomUpDeviceClustering> {
public:
  using DeviceClusteringAlgoBase::DeviceClusteringAlgoBase;
  void mergeDeviceClustersProgressively();
};

void BottomUpDeviceClustering::mergeDeviceClustersProgressively() {
  SmallVector<Operation *> ops;
  for (auto &op : llvm::reverse(funcOp.front().without_terminator()))
    ops.push_back(&op);

  for (auto op : ops) {
    auto curCluster = getCluster(op);
    for (auto &&use : op->getUses()) {
      auto preCluster = getCluster(use.getOwner());
      if (auto merged = ActiveDeviceCluster::tryMerge(preCluster, curCluster,
                                                      op2cluster)) {
        curCluster = merged;
      }
    }
  }
}

void createFunctions(ModuleOp module_op,
                     SmallVector<FunctionMetadata, 4> &metadatas,
                     StringRef attrName) {
  MLIRContext *context = module_op.getContext();
  SymbolTable symbolTable(module_op);
  for (auto &metadata : metadatas) {
    llvm::SmallVector<mlir::Type, 4> inputTypes;
    llvm::SmallVector<mlir::Type, 4> resultTypes;
    for (Value input : metadata.inputs) {
      inputTypes.push_back(input.getType());
    }
    for (Value result : metadata.results) {
      resultTypes.push_back(result.getType());
    }
    std::string funcName =
        (metadata.originalName + "_" + metadata.deviceAttr).str();
    FunctionType funcType = FunctionType::get(context, inputTypes, resultTypes);
    func::FuncOp funcOp =
        func::FuncOp::create(UnknownLoc::get(context), funcName, funcType);
    funcOp->setAttr(attrName, StringAttr::get(context, metadata.deviceAttr));
    funcOp->setAttr(metadata.anchorName, UnitAttr::get(context));
    funcOp.setPublic();
    Block *block = funcOp.addEntryBlock();

    // Clones and moves the operations into the function's body. And the cloned
    // operation should use the arguments of the newly created funcOp as
    // appropriate.
    OpBuilder builder(block, block->end());
    IRMapping mapping;
    for (int i : llvm::seq<int>(0, metadata.inputs.size())) {
      Value originalValue = metadata.inputs[i];
      Value newValue = funcOp.getArgument(i);
      mapping.map(originalValue, newValue);
    }
    for (Operation *op : metadata.ops) {
      builder.clone(*op, mapping);
    }
    // Creates the ReturnOp so that the per-host function returns the
    // correct values of the cloned operations.
    llvm::SmallVector<Value, 4> resultsAfterMapping;
    for (Value result : metadata.results) {
      resultsAfterMapping.push_back(mapping.lookupOrDefault(result));
    }
    builder.create<func::ReturnOp>(UnknownLoc::get(context),
                                   resultsAfterMapping);
    symbolTable.insert(funcOp, metadata.insertionPoint++);
    // Record the actual name. The symbol table might rename the FuncOp if there
    // is name collision.
    metadata.partitionName = funcOp.getName();
    metadata.partitionOp = funcOp;
  }
}

void createCalls(MLIRContext *context,
                 const SmallVector<FunctionMetadata, 4> &metadatas,
                 Operation *retOp, bool dupOutputs) {
  IRMapping mapping;
  for (auto &metadata : metadatas) {
    // Creates the CallOp.
    OpBuilder builder(metadata.ops.back());
    llvm::SmallVector<Type, 4> resultTypes;
    for (Value result : metadata.results) {
      resultTypes.push_back(result.getType());
    }
    llvm::SmallVector<Value, 4> inputsAfterMapping;
    for (Value input : metadata.inputs) {
      inputsAfterMapping.push_back(mapping.lookupOrDefault(input));
    }

    func::CallOp callOp = builder.create<func::CallOp>(
        UnknownLoc::get(context), metadata.partitionOp, inputsAfterMapping);
    // Clones the CallOp operation to replace its callee args with
    // the results of the other CallOp operations using the
    // `mapping` as appropriate.
    Operation *clonedCallOp = builder.clone(*callOp.getOperation(), mapping);
    callOp.erase();

    llvm::DenseMap<Value, SmallVector<int>> retOperand2Indices;
    for (int i = retOp->getNumOperands() - 1; i >= 0; --i) {
      Value value = retOp->getOperand(i);
      retOperand2Indices[value].push_back(i);
    }

    // Replaces usages of the results of the original operations with the
    // results of the CallOp operations.
    for (int i : llvm::seq<int>(0, metadata.results.size())) {
      Value originalValue = metadata.results[i];
      Value newValue = clonedCallOp->getResult(i);
      if (dupOutputs) {
        originalValue.replaceAllUsesExcept(newValue, retOp);
        if (retOperand2Indices.find(originalValue) !=
            retOperand2Indices.end()) {
          assert(retOperand2Indices[originalValue].size() > 0 &&
                 "Corresponding indices vector must not be empty");
          int idx = retOperand2Indices[originalValue].back();
          retOperand2Indices[originalValue].pop_back();
          retOp->getOpOperand(idx).set(newValue);
        }
      } else {
        originalValue.replaceAllUsesWith(newValue);
      }
      mapping.map(originalValue, newValue);
    }
  }
}

mlir::LogicalResult
GraphClustingByDevice(ModuleOp moduleOp, std::string attrName,
                      std::string device, std::string deviceAnchorName,
                      bool dupNonSplat, bool dupOutputs,
                      GraphClusteringAlgo clusterAlgo, bool enableMultiGraph,
                      ValidateSubGraphFn validateSubGraphFn) {
  MLIRContext *context = moduleOp.getContext();
  SmallVector<func::FuncOp, 4> originalFuncs;
  const auto isResultUsedByReturnOp =
      [](Operation *op, llvm::SmallDenseSet<Value> &retValues) {
        return llvm::any_of(op->getResults(), [&retValues](Value v) {
          return retValues.count(v) > 0;
        });
      };
  for (auto funcOp : moduleOp.getOps<func::FuncOp>()) {
    llvm::SmallDenseSet<Value> retValues;
    for (const auto &operand : funcOp.front().back().getOperands()) {
      retValues.insert(operand);
    }
    for (auto &block : funcOp.getBlocks()) {
      if (dupNonSplat)
        replicateDefiningOp(
            &block, [&retValues, &isResultUsedByReturnOp](Operation *op) {
              return !isResultUsedByReturnOp(op, retValues) &&
                     isMhloConstantLike(op);
            });
      else
        replicateDefiningOp(
            &block, [&retValues, &isResultUsedByReturnOp](Operation *op) {
              return !isResultUsedByReturnOp(op, retValues) &&
                     isSplatMhloConstantLike(op);
            });
    }
    originalFuncs.push_back(funcOp);
  }
  for (auto funcOp : originalFuncs) {
    std::optional<SmallVector<FunctionMetadata, 4>> metadatas;
    switch (clusterAlgo) {
    case GraphClusteringAlgo::kTopDown: {
      metadatas = TopDownDeviceClustering(funcOp, attrName)
                      .getFunctionMetadatas(attrName, device, deviceAnchorName,
                                            dupOutputs, enableMultiGraph,
                                            validateSubGraphFn);
      break;
    }
    case GraphClusteringAlgo::kBottomUp: {
      metadatas = BottomUpDeviceClustering(funcOp, attrName)
                      .getFunctionMetadatas(attrName, device, deviceAnchorName,
                                            dupOutputs, enableMultiGraph,
                                            validateSubGraphFn);
      break;
    }
    case GraphClusteringAlgo::kGreedy: {
      std::optional<SmallVector<FunctionMetadata, 4>> topDownMetadatas;
      std::optional<SmallVector<FunctionMetadata, 4>> bottomUpMetadatas;
      auto topDownFunc = funcOp.clone();
      auto bottomUpFunc = funcOp.clone();

      topDownMetadatas = TopDownDeviceClustering(topDownFunc, attrName)
                             .getFunctionMetadatas(
                                 attrName, device, deviceAnchorName, dupOutputs,
                                 enableMultiGraph, validateSubGraphFn);
      bottomUpMetadatas =
          BottomUpDeviceClustering(bottomUpFunc, attrName)
              .getFunctionMetadatas(attrName, device, deviceAnchorName,
                                    dupOutputs, enableMultiGraph,
                                    validateSubGraphFn);
      if (topDownMetadatas && bottomUpMetadatas) {
        size_t topDownSize = std::accumulate(
            (*topDownMetadatas).begin(), (*topDownMetadatas).end(), 0,
            [](size_t val, const FunctionMetadata &metadata) {
              return val + metadata.ops.size();
            });
        size_t bottomUpSize = std::accumulate(
            (*bottomUpMetadatas).begin(), (*bottomUpMetadatas).end(), 0,
            [](size_t val, const FunctionMetadata &metadata) {
              return val + metadata.ops.size();
            });

        if (topDownSize > bottomUpSize) {
          metadatas = TopDownDeviceClustering(funcOp, attrName)
                          .getFunctionMetadatas(
                              attrName, device, deviceAnchorName, dupOutputs,
                              enableMultiGraph, validateSubGraphFn);
        } else {
          metadatas = BottomUpDeviceClustering(funcOp, attrName)
                          .getFunctionMetadatas(
                              attrName, device, deviceAnchorName, dupOutputs,
                              enableMultiGraph, validateSubGraphFn);
        }
      } else if (topDownMetadatas) {
        metadatas = TopDownDeviceClustering(funcOp, attrName)
                        .getFunctionMetadatas(
                            attrName, device, deviceAnchorName, dupOutputs,
                            enableMultiGraph, validateSubGraphFn);
      } else if (bottomUpMetadatas) {
        metadatas = BottomUpDeviceClustering(funcOp, attrName)
                        .getFunctionMetadatas(
                            attrName, device, deviceAnchorName, dupOutputs,
                            enableMultiGraph, validateSubGraphFn);
      }
      topDownFunc.erase();
      bottomUpFunc.erase();
      break;
    }
    case GraphClusteringAlgo::kFallback:
    default: {
      metadatas = getFunctionMetadatasFallback(funcOp, attrName, device,
                                               deviceAnchorName, dupOutputs,
                                               validateSubGraphFn);
    }
    }

    if (!metadatas) {
      funcOp->emitError()
          << "[ByteIR Transform]: GraphClusteringByDevice error.";
      return failure();
    }

    Operation &retOp = funcOp.front().back();
    createFunctions(moduleOp, *metadatas, attrName);
    createCalls(context, *metadatas, &retOp, dupOutputs);

    // Erases the original operations which have been cloned in the partitioned
    // functions.
    for (auto &metadata : *metadatas) {
      for (int i = static_cast<int>(metadata.ops.size()) - 1; i >= 0; i--) {
        metadata.ops[i]->erase();
      }
    }
  }
  return success();
}

struct GraphClusteringByDevicePass
    : public GraphClusteringByDeviceBase<GraphClusteringByDevicePass> {

  explicit GraphClusteringByDevicePass(std::string attrName, std::string device,
                                       std::string deviceAnchorName,
                                       bool dupNonSplat, bool dupOutputs,
                                       GraphClusteringAlgo clusterAlgo,
                                       bool enableMultiGraph,
                                       ValidateSubGraphFn validateSubGraphFn)
      : GraphClusteringByDeviceBase<
            GraphClusteringByDevicePass>::GraphClusteringByDeviceBase() {
    this->attrName = attrName;
    this->device = device;
    this->deviceAnchorName = deviceAnchorName;
    this->dupNonSplat = dupNonSplat;
    this->dupOutputs = dupOutputs;
    this->clusterAlgo = clusterAlgo;
    this->enableMultiGraph = enableMultiGraph;
    this->validateSubGraphFn = validateSubGraphFn;
  }

  void runOnOperation() override;

  ValidateSubGraphFn validateSubGraphFn = nullptr;
};

void GraphClusteringByDevicePass::runOnOperation() {
  ModuleOp moduleOp = getOperation();
  MLIRContext *context = &getContext();
  if (failed(GraphClustingByDevice(moduleOp, attrName, device, deviceAnchorName,
                                   dupNonSplat, dupOutputs, clusterAlgo,
                                   enableMultiGraph, validateSubGraphFn))) {
    signalPassFailure();
  }
}

} // namespace

std::unique_ptr<OperationPass<ModuleOp>>
mlir::createGraphClusteringByDevicePass(
    std::string attrName, std::string device, std::string deviceAnchorName,
    bool dupNonSplat, bool dupOutputs, GraphClusteringAlgo clusterAlgo,
    bool enableMultiGraph, ValidateSubGraphFn validateSubGraphFn) {
  return std::make_unique<GraphClusteringByDevicePass>(
      attrName, device, deviceAnchorName, dupNonSplat, dupOutputs, clusterAlgo,
      enableMultiGraph, validateSubGraphFn);
}
