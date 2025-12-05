const std = @import("std");

pub const Vm2vmType = enum {
    disabled,
    software,
    hardware,
    last,
};

pub const Config = struct {
    enabled_port_mask: u32 = 0,
    promiscuous: u32,
    num_queues: u32 = 0,
    num_devices: u32,
    // mbuf_pool: *rte_mempool = null,
    mergeable: u32,
    vm2vm_mode: Vm2vmType = .software,

    // flags
    enable_stats: u32 = 0,
    enable_retry: u32 = 1,
    enable_tx_csum: u32,
    enable_tso: u32,
    client_mode: u32,
    dequeue_zero_copy: u32,
    builtin_net_driver: u32,

    burst_rx_delay_time: u32 = 15,
    burst_rx_retry_num: u32 = 4,

    socket_files: []const u8,
};
