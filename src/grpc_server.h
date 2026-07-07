#ifndef GRPC_SERVER_H
#define GRPC_SERVER_H

#include "args.h"

// Runs the gRPC CallAgent service (blocking) until the idle watchdog fires.
// Returns the process exit code.
int runGrpcServer(const Config& config);

#endif // GRPC_SERVER_H
