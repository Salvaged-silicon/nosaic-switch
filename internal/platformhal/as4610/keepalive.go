package as4610

import "runtime"

// runtimeKeepAlive keeps its arguments reachable until it returns.
//
// The ioctl above passes the addresses of Go-allocated buffers to the kernel
// as plain integers, and the compiler is entitled to consider a slice dead
// after its last Go-visible use — which is before the syscall it is being read
// through. Nothing has gone wrong in practice and nothing would announce it if
// it did: the kernel would read freed memory, and what came back would be
// wrong rather than absent.
func runtimeKeepAlive(v ...any) {
	for _, x := range v {
		runtime.KeepAlive(x)
	}
}
