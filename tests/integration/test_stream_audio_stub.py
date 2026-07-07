"""StreamAudio — PoC returns UNIMPLEMENTED."""

import grpc


def test_stream_audio_stub(proto_stubs, agent_server):
    pb2, pb2_grpc = proto_stubs

    grpc_port = agent_server()

    channel = grpc.insecure_channel(f"127.0.0.1:{grpc_port}")
    stub = pb2_grpc.CallAgentStub(channel)

    # For bidi streaming, the stub returns a response iterator.
    # We must consume it to trigger the RPC.
    responses = stub.StreamAudio(iter([]), timeout=5)
    try:
        for _ in responses:
            pass
        assert False, "Expected UNIMPLEMENTED"
    except grpc.RpcError as e:
        assert e.code() == grpc.StatusCode.UNIMPLEMENTED, \
            f"Expected UNIMPLEMENTED, got {e.code()}"
