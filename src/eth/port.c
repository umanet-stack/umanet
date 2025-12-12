/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2017 Intel Corporation
 */

#include <rte_ethdev.h>
#include <stdint.h>

#include "../include/tas.h"
#include "src/config/config.h"
#include "src/eth/eth.h"

/* Configurable number of RX/TX ring descriptors */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 512

#define INVALID_PORT_ID 0xFF

eth_state_t eth = {
    .num_queues = 0,
    .vlan_tags =
        {
            1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015,
            1016, 1017, 1018, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031,
            1032, 1033, 1034, 1035, 1036, 1037, 1038, 1039, 1040, 1041, 1042, 1043, 1044, 1045, 1046, 1047,
            1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063,
        },
};

uint32_t port = 0;

/*
 * Update the global var NUM_PORTS and array PORTS according to system ports number
 * and return valid ports number
 */
unsigned check_ports_num(unsigned nb_ports) {
    unsigned valid_num_ports = config.num_ports;
    unsigned portid;

    if (config.num_ports > nb_ports) {
        RTE_LOG(INFO, VHOST_PORT, "\nSpecified port number(%u) exceeds total system port number(%u)\n",
                config.num_ports, nb_ports);
        config.num_ports = nb_ports;
    }

    for (portid = 0; portid < config.num_ports; portid++) {
        if (!rte_eth_dev_is_valid_port(config.ports[portid])) {
            RTE_LOG(INFO, VHOST_PORT, "\nSpecified port ID(%u) is not valid\n", config.ports[portid]);
            config.ports[portid] = INVALID_PORT_ID;
            valid_num_ports--;
        }
    }
    return valid_num_ports;
}