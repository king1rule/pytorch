#include <torch/csrc/distributed/rpc/python_functions.h>

#include <c10/util/C++17.h>
#include <torch/csrc/distributed/autograd/context/container.h>
#include <torch/csrc/distributed/autograd/utils.h>
#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/python_call.h>
#include <torch/csrc/distributed/rpc/python_remote_call.h>
#include <torch/csrc/distributed/rpc/python_resp.h>
#include <torch/csrc/distributed/rpc/python_rpc_handler.h>
#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/script_remote_call.h>
#include <torch/csrc/distributed/rpc/script_resp.h>
#include <torch/csrc/distributed/rpc/utils.h>
#include <torch/csrc/jit/pybind_utils.h>

namespace torch {
namespace distributed {
namespace rpc {

namespace {

std::shared_ptr<Operator> matchBuiltinOp(
    const std::string& opName,
    const py::args& args,
    const py::kwargs& kwargs,
    Stack& stack) {
  Symbol symbol = Symbol::fromQualString(opName);
  if (symbol.is_aten()) {
    for (const auto& op : torch::jit::getAllOperatorsFor(symbol)) {
      try {
        // FIXME: This is temporary solution. We should at least refactor
        // ``createStackForSchema`` to avoid throwing an error.
        stack = torch::jit::createStackForSchema(
            op->schema(), args, kwargs, c10::nullopt);
      } catch (std::runtime_error& e) {
        VLOG(1) << "Couldn't match schema: " << op->schema()
                << " to args: " << args << " and kwargs: " << kwargs
                << ", reason: " << e.what();
        continue;
      }

      // Found the right op!
      return op;
    }
  }

  TORCH_CHECK(
      false,
      "Failed to match operator name ",
      opName,
      " and arguments "
      "(args: ",
      args,
      ", kwargs: ",
      kwargs,
      ") to a builtin operator");
}

void finishCreatingOwnerRRef(
    const Message& message,
    const c10::optional<utils::FutureError>& futErr) {
  RRefContext::handleException(futErr);
  auto rr = RemoteRet::fromMessage(message);
  TORCH_INTERNAL_ASSERT(
      rr->rrefId() == rr->forkId(),
      "Expecting an OwnerRRef as RemoteRet but got a fork.");
  auto& ctx = RRefContext::getInstance();
  ctx.delForkOfOwner(rr->rrefId(), rr->rrefId());
}

std::shared_ptr<FutureMessage> sendPythonRemoteCall(
    RpcAgent& agent,
    const WorkerInfo& dst,
    SerializedPyObj serializedPyObj,
    const IValue& rrefId,
    const IValue& forkId,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf) {
  auto pythonRemoteCall = std::make_unique<PythonRemoteCall>(
      std::move(serializedPyObj), rrefId, forkId);

  // set forceGradRecording to true as even if the args does not contain any
  // tensor, the return value might still contain tensors.
  return torch::distributed::autograd::sendMessageWithAutograd(
      agent,
      dst,
      std::move(*pythonRemoteCall).toMessage(),
      true /*forceGradRecording*/,
      rf);
}

} // namespace

using namespace torch::distributed::autograd;

py::object toPyObjInternal(RpcCommandBase& rpc, MessageType messageType) {
  switch (messageType) {
    case MessageType::SCRIPT_RET: {
      auto& ret = static_cast<ScriptResp&>(rpc);
      Stack stack;
      stack.push_back(ret.value());
      {
        pybind11::gil_scoped_acquire ag;
        // The createPyObjectForStack does not acquire GIL, but creating a new
        // py::object requires GIL.
        return torch::jit::createPyObjectForStack(std::move(stack));
      }
    }
    case MessageType::PYTHON_RET: {
      // TODO: Try to avoid a copy here.
      auto& resp = static_cast<PythonResp&>(rpc);

      return PythonRpcHandler::getInstance().loadPythonUDFResult(
          resp.pickledPayload(), resp.tensors());
    }
    default: {
      TORCH_CHECK(false, "Unrecognized response message type ", messageType);
    }
  }
}

py::object toPyObj(const Message& message) {
  MessageType msgType = message.type();
  auto response = deserializeResponse(message, msgType);
  return toPyObjInternal(*response, msgType);
}

std::shared_ptr<FutureMessage> pyRpcBuiltin(
    RpcAgent& agent,
    const WorkerInfo& dst,
    const std::string& opName,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf,
    const py::args& args,
    const py::kwargs& kwargs) {
  Stack stack;
  auto op = matchBuiltinOp(opName, args, kwargs, stack);
  auto scriptCall = std::make_unique<ScriptCall>(op, std::move(stack));
  return sendMessageWithAutograd(
      agent, dst, std::move(*scriptCall).toMessage(), false, rf);
}

PyRRef pyRemoteBuiltin(
    RpcAgent& agent,
    const WorkerInfo& dst,
    const std::string& opName,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf,
    const py::args& args,
    const py::kwargs& kwargs) {
  Stack stack;
  auto op = matchBuiltinOp(opName, args, kwargs, stack);
  TypePtr returnType = op->schema().returns()[0].type();

  auto& ctx = RRefContext::getInstance();
  // TODO: support creating RRefs on a local object.
  TORCH_INTERNAL_ASSERT(
      ctx.getWorkerId() != dst.id_,
      "Does not support creating RRef on self yet.");
  auto userRRef = ctx.createUserRRef(dst.id_, returnType);

  auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
      op, std::move(stack), userRRef->rrefId(), userRRef->forkId());

  auto fm = sendMessageWithAutograd(
      agent, dst, std::move(*scriptRemoteCall).toMessage(), false, rf);

  ctx.addPendingUser(userRRef->forkId(), userRRef);
  fm->addCallback(callback::confirmPendingUser);
  return PyRRef(userRRef);
}

std::shared_ptr<FutureMessage> pyRpcPythonUdf(
    RpcAgent& agent,
    const WorkerInfo& dst,
    std::string& pickledPythonUDF,
    std::vector<torch::Tensor>& tensors,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf) {
  auto pythonCall = std::make_unique<PythonCall>(
      std::vector<char>(pickledPythonUDF.begin(), pickledPythonUDF.end()),
      tensors);
  return sendMessageWithAutograd(
      agent,
      dst,
      std::move(*pythonCall).toMessage(),
      true /*forceGradRecording*/,
      rf);
}

PyRRef pyRemotePythonUdf(
    RpcAgent& agent,
    const WorkerInfo& dst,
    std::string& pickledPythonUDF,
    std::vector<torch::Tensor>& tensors,
    const std::shared_ptr<torch::autograd::profiler::RecordFunction>& rf) {
  auto& ctx = RRefContext::getInstance();
  auto serializedPyObj =
      SerializedPyObj(std::move(pickledPythonUDF), std::move(tensors));
  if (ctx.getWorkerId() != dst.id_) {
    auto userRRef = ctx.createUserRRef(dst.id_, PyObjectType::get());
    ctx.addPendingUser(userRRef->forkId(), userRRef);
    auto fm = sendPythonRemoteCall(
        agent,
        dst,
        std::move(serializedPyObj),
        userRRef->rrefId().toIValue(),
        userRRef->forkId().toIValue(),
        rf);

    fm->addCallback(callback::confirmPendingUser);
    return PyRRef(userRRef);
  } else {
    auto ownerRRef = ctx.createOwnerRRef(PyObjectType::get());
    // prevent this owner RRef be deleted due to other forks
    ctx.addSelfAsFork(ownerRRef);
    auto fm = sendPythonRemoteCall(
        agent,
        dst,
        std::move(serializedPyObj),
        ownerRRef->rrefId().toIValue(),
        ownerRRef->rrefId().toIValue(),
        rf);

    fm->addCallback(finishCreatingOwnerRRef);
    return PyRRef(ownerRRef);
  }
}

} // namespace rpc
} // namespace distributed
} // namespace torch
