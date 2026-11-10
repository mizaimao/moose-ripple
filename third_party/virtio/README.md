# Vendored virtio headers

From Linux v6.12, `include/uapi/linux/`, fetched 2026-11-10 from
`https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/<file>`.
Each file states it is BSD licensed "so anyone can use the definitions to implement compatible
drivers/servers"; the license text is kept in each file. Unmodified, except that
`linux/types.h` is our own small shim, not Linux's.

| File | SHA-256 |
|---|---|
| `virtio_config.h` | `00291a2835f3011b420077c8bddb5bfacfc65b982a8c85e29d5574dee8913ebd` |
| `virtio_gpu.h` | `7c9e2f7d47fa0b1a2c737fc5a741f57c5cf25303dd5c68c2c9738e9bb761eee6` |
| `virtio_pci.h` | `1a3715398b81436bf7c4fb2f66f9be3f13082eea08e1e419938264131ed6dd3e` |
| `virtio_ring.h` | `57fd059ebd45422fce646bc81d9c6a148f6d9dc15354573f389d53264dec193c` |
| `virtio_types.h` | `3a6bf6debdb7873b234ca57693a61a77b6b43c68b90d60bfc14ae5fcce5d6d09` |
