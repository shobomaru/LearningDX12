CFLAGS = -std=c++20 -I../DirectX-Headers/include -I../DirectX-Headers/include/wsl/stubs
LDFLAGS = -L/usr/lib/wsl/lib
LIBS = -ld3d12 -ld3d12core -ldxcore

HelloWSL2: HelloWSL2.cpp
	g++ $(CFLAGS) $(LDFLAGS) -o HelloWSL2 HelloWSL2.cpp $(LIBS)

clean: rm -f *.o HelloWSL2 image.ppm

