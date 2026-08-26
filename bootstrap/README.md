# bootstrap/ — cross-toolchains

crosstool-NG configurations, one per architecture, published as versioned artifacts
so that nobody rebuilds GCC in order to build a package.

x86_64 goes through crosstool-NG too, even though the build host is x86_64. Taking
the same path as every other architecture is what stops host assumptions leaking in
and only being discovered when the first cross-build is attempted.

Lands at M1.
