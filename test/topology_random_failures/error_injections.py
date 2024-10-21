#
# Copyright (C) 2024-present ScyllaDB
#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

# - New items should be added to the end of the list
# - Items in the following list should not be rearranged or deleted
ERROR_INJECTIONS = (
    "stop_after_init_of_system_ks",
    "stop_after_init_of_schema_commitlog",
    "stop_after_starting_gossiper",
    "stop_after_starting_raft_address_map",
    "stop_after_starting_migration_manager",
    "stop_after_starting_commitlog",
    "stop_after_starting_repair",
    "stop_after_starting_cdc_generation_service",
    "stop_after_starting_group0_service",
    "stop_after_starting_auth_service",
    "stop_during_gossip_shadow_round",
    "stop_after_saving_tokens",
    "stop_after_starting_gossiping",
    "stop_after_sending_join_node_request",
    "stop_after_setting_mode_to_normal_raft_topology",
    "stop_before_becoming_raft_voter",
    "stop_after_updating_cdc_generation",
    "stop_before_streaming",
    "stop_after_streaming",
    "stop_after_bootstrapping_initial_raft_configuration",
)