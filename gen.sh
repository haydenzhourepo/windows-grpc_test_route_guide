# build from source
## export environment variable
export PATH=$PATH:~/soft/grpc1_51/bin

## generate grpc server and client stub code
protoc.exe --grpc_out=./proto --plugin=protoc-gen-grpc=/c/Users/mfkha/soft/grpc1_51/bin/grpc_cpp_plugin.exe route_guide.proto

## generate grpc message serialisable code
protoc.exe --cpp_out=./proto route_guide.proto



# installed by vcpkg
## export environment variable
export PATH=$PATH:~/soft/vcpkg/installed/x64-windows/tools/grpc
export PATH=$PATH:~/soft/vcpkg/installed/x64-windows/tools/protobuf

## generate grpc server and client stub code
protoc.exe --grpc_out=./proto --plugin=protoc-gen-grpc=/c/Users/mfkha/soft/vcpkg/installed/x64-windows/tools/grpc/grpc_cpp_plugin.exe route_guide.proto

## generate grpc message serialisable code
protoc.exe --cpp_out=./proto route_guide.proto