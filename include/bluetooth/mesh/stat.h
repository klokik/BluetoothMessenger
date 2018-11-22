/** @file
 *  @brief Bluetooth Mesh Statistics APIs.
 */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_BLUETOOTH_MESH_STAT_H_
#define ZEPHYR_INCLUDE_BLUETOOTH_MESH_STAT_H_

/**
 * @brief Bluetooth Mesh Statistics
 * @defgroup bt_mesh_stat Bluetooth Mesh Statistics
 * @ingroup bt_mesh
 * @{
 */

typedef void (*bt_mesh_stat_relay_cb_t)(u16_t net_idx, u16_t src, u16_t dst,
					u8_t ttl);

void bt_mesh_stat_set_relay_cb(bt_mesh_stat_relay_cb_t cb);

/**
 * @}
 */

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_MESH_STAT_H_ */
