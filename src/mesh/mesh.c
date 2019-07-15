/*
 * Copyright (C) 2019 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "mesh.c"

#include <string.h>
#include <stdio.h>

#include "mesh/mesh.h"

#include "btstack_util.h"
#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_tlv.h"
#include "btstack_memory.h"

#include "mesh/adv_bearer.h"
#include "mesh/beacon.h"
#include "mesh/gatt_bearer.h"
#include "mesh/mesh_access.h"
#include "mesh/mesh_configuration_server.h"
#include "mesh/mesh_foundation.h"
#include "mesh/mesh_generic_model.h"
#include "mesh/mesh_generic_server.h"
#include "mesh/mesh_iv_index_seq_number.h"
#include "mesh/mesh_lower_transport.h"
#include "mesh/mesh_peer.h"
#include "mesh/mesh_proxy.h"
#include "mesh/mesh_upper_transport.h"
#include "mesh/mesh_virtual_addresses.h"
#include "mesh/pb_adv.h"
#include "mesh/pb_gatt.h"
#include "mesh/provisioning.h"
#include "mesh/provisioning_device.h"

// Persistent storage structures

typedef struct {
    uint16_t netkey_index;

    uint8_t  version;

    // net_key from provisioner or Config Model Client
    uint8_t net_key[16];

    // derived data

    // k1
    uint8_t identity_key[16];
    uint8_t beacon_key[16];

    // k3
    uint8_t network_id[8];

    // k2
    uint8_t nid;
    uint8_t encryption_key[16];
    uint8_t privacy_key[16];
} mesh_persistent_net_key_t;

typedef struct {
    uint16_t netkey_index;
    uint16_t appkey_index;
    uint8_t  aid;
    uint8_t  version;
    uint8_t  key[16];
} mesh_persistent_app_key_t;

static btstack_packet_handler_t provisioning_device_packet_handler;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static int provisioned;

// Mandatory Confiuration Server 
static mesh_model_t                 mesh_configuration_server_model;

// Mandatory Health Server
static mesh_model_t                 mesh_health_server_model;
static mesh_configuration_server_model_context_t mesh_configuration_server_model_context;

// Random UUID on start
static btstack_crypto_random_t mesh_access_crypto_random;
static uint8_t random_device_uuid[16];

// TLV
static const btstack_tlv_t * btstack_tlv_singleton_impl;
static void *                btstack_tlv_singleton_context;

void mesh_access_setup_from_provisioning_data(const mesh_provisioning_data_t * provisioning_data){

    // set iv_index and iv index update active
    int iv_index_update_active = (provisioning_data->flags & 2) >> 1;
    mesh_iv_index_recovered(iv_index_update_active, provisioning_data->iv_index);

    // set unicast address
    mesh_node_primary_element_address_set(provisioning_data->unicast_address);

    // set device_key
    mesh_transport_set_device_key(provisioning_data->device_key);

    if (provisioning_data->network_key){

        // setup primary network with provisioned netkey
        mesh_network_key_add(provisioning_data->network_key);

        // setup primary network
        mesh_subnet_setup_for_netkey_index(provisioning_data->network_key->netkey_index);

        // start sending Secure Network Beacons
        mesh_subnet_t * provisioned_subnet = mesh_subnet_get_by_netkey_index(provisioning_data->network_key->netkey_index);
        beacon_secure_network_start(provisioned_subnet);
    }

    // Mesh Proxy
#ifdef ENABLE_MESH_PROXY_SERVER
    // Setup Proxy
    mesh_proxy_init(provisioning_data->unicast_address);
    mesh_proxy_start_advertising_with_network_id();
#endif
}

static void mesh_access_setup_unprovisioned_device(void * arg){
    // set random value
    if (arg == NULL){
        mesh_node_set_device_uuid(random_device_uuid);
    }

#ifdef ENABLE_MESH_PB_ADV
    // PB-ADV    
    beacon_unprovisioned_device_start(mesh_node_get_device_uuid(), 0);
#endif
#ifdef ENABLE_MESH_PB_GATT
    mesh_proxy_start_advertising_unprovisioned_device();
#endif
}

void mesh_access_setup_without_provisiong_data(void){
    const uint8_t * device_uuid = mesh_node_get_device_uuid();
    if (device_uuid){
        mesh_access_setup_unprovisioned_device((void *)device_uuid);
    } else{
        btstack_crypto_random_generate(&mesh_access_crypto_random, random_device_uuid, 16, &mesh_access_setup_unprovisioned_device, NULL);
    }
}

static void mesh_provisioning_message_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    mesh_provisioning_data_t provisioning_data;

    switch(packet[0]){
        case HCI_EVENT_MESH_META:
            switch(packet[2]){
                case MESH_SUBEVENT_PB_PROV_COMPLETE:
                    // get provisioning data
                    provisioning_device_data_get(&provisioning_data);

                    // and store in TLV
                    mesh_node_store_provisioning_data(&provisioning_data);

                    // setup node after provisioned
                    mesh_access_setup_from_provisioning_data(&provisioning_data);

                    // start advertising with node id after provisioning
                    mesh_proxy_set_advertising_with_node_id(provisioning_data.network_key->netkey_index, MESH_NODE_IDENTITY_STATE_ADVERTISING_RUNNING);

                    provisioned = 1;
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    if (provisioning_device_packet_handler == NULL) return;

    // forward
    (*provisioning_device_packet_handler)(packet_type, channel, packet, size);
}

static void hci_packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
                    // get TLV instance
                    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);

                    // startup from provisioning data stored in TLV
                    provisioned = mesh_node_startup_from_tlv();
                    break;
                
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    // enable PB_GATT
                    if (provisioned == 0){
                        mesh_proxy_start_advertising_unprovisioned_device();
                    } else {
#ifdef ENABLE_MESH_PROXY_SERVER
                        mesh_proxy_start_advertising_with_network_id();
#endif
                    }
                    break;
                    
                case HCI_EVENT_LE_META:
                    if (hci_event_le_meta_get_subevent_code(packet) !=  HCI_SUBEVENT_LE_CONNECTION_COMPLETE) break;
                    // disable PB_GATT
                    mesh_proxy_stop_advertising_unprovisioned_device();
                    break;
                default:
                    break;
            }
            break;
    }
}


// Mesh Network Keys
static uint32_t mesh_network_key_tag_for_internal_index(uint16_t internal_index){
    return ((uint32_t) 'M' << 24) | ((uint32_t) 'N' << 16) | ((uint32_t) internal_index);
}

void mesh_store_network_key(mesh_network_key_t * network_key){
    mesh_persistent_net_key_t data;
    printf("Store NetKey: internal index 0x%x, NetKey Index 0x%06x, NID %02x: ", network_key->internal_index, network_key->netkey_index, network_key->nid);
    printf_hexdump(network_key->net_key, 16);
    uint32_t tag = mesh_network_key_tag_for_internal_index(network_key->internal_index);
    data.netkey_index = network_key->netkey_index;
    memcpy(data.net_key, network_key->net_key, 16);
    memcpy(data.identity_key, network_key->identity_key, 16);
    memcpy(data.beacon_key, network_key->beacon_key, 16);
    memcpy(data.network_id, network_key->network_id, 8);
    data.nid = network_key->nid;
    data.version = network_key->version;
    memcpy(data.encryption_key, network_key->encryption_key, 16);
    memcpy(data.privacy_key, network_key->privacy_key, 16);
    btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, tag, (uint8_t *) &data, sizeof(mesh_persistent_net_key_t));
}

void mesh_delete_network_key(uint16_t internal_index){
    uint32_t tag = mesh_network_key_tag_for_internal_index(internal_index);
    btstack_tlv_singleton_impl->delete_tag(btstack_tlv_singleton_context, tag);
}


void mesh_load_network_keys(void){
    printf("Load Network Keys\n");
    uint16_t internal_index;
    for (internal_index = 0; internal_index < MAX_NR_MESH_NETWORK_KEYS; internal_index++){
        mesh_persistent_net_key_t data;
        uint32_t tag = mesh_network_key_tag_for_internal_index(internal_index);
        int netkey_len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, tag, (uint8_t *) &data, sizeof(data));
        if (netkey_len != sizeof(mesh_persistent_net_key_t)) continue;
        
        mesh_network_key_t * network_key = btstack_memory_mesh_network_key_get();
        if (network_key == NULL) return;

        network_key->netkey_index = data.netkey_index;
        memcpy(network_key->net_key, data.net_key, 16);
        memcpy(network_key->identity_key, data.identity_key, 16);
        memcpy(network_key->beacon_key, data.beacon_key, 16);
        memcpy(network_key->network_id, data.network_id, 8);
        network_key->nid = data.nid;
        network_key->version = data.version;
        memcpy(network_key->encryption_key, data.encryption_key, 16);
        memcpy(network_key->privacy_key, data.privacy_key, 16);

#ifdef ENABLE_GATT_BEARER
        // setup advertisement with network id
        network_key->advertisement_with_network_id.adv_length = mesh_proxy_setup_advertising_with_network_id(network_key->advertisement_with_network_id.adv_data, network_key->network_id);
#endif

        mesh_network_key_add(network_key);

        mesh_subnet_setup_for_netkey_index(network_key->netkey_index);

        printf("- internal index 0x%x, NetKey Index 0x%06x, NID %02x: ", network_key->internal_index, network_key->netkey_index, network_key->nid);
        printf_hexdump(network_key->net_key, 16);
    }
}

void mesh_delete_network_keys(void){
    printf("Delete Network Keys\n");
    
    uint16_t internal_index;
    for (internal_index = 0; internal_index < MAX_NR_MESH_NETWORK_KEYS; internal_index++){
        mesh_delete_network_key(internal_index);
    }
}

// Mesh App Keys

static uint32_t mesh_transport_key_tag_for_internal_index(uint16_t internal_index){
    return ((uint32_t) 'M' << 24) | ((uint32_t) 'A' << 16) | ((uint32_t) internal_index);
}

void mesh_store_app_key(mesh_transport_key_t * app_key){
    mesh_persistent_app_key_t data;
    printf("Store AppKey: internal index 0x%x, AppKey Index 0x%06x, AID %02x: ", app_key->internal_index, app_key->appkey_index, app_key->aid);
    printf_hexdump(app_key->key, 16);
    uint32_t tag = mesh_transport_key_tag_for_internal_index(app_key->internal_index);
    data.netkey_index = app_key->netkey_index;
    data.appkey_index = app_key->appkey_index;
    data.aid = app_key->aid;
    data.version = app_key->version;
    memcpy(data.key, app_key->key, 16);
    btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, tag, (uint8_t *) &data, sizeof(data));
}

void mesh_delete_app_key(uint16_t internal_index){
    uint32_t tag = mesh_transport_key_tag_for_internal_index(internal_index);
    btstack_tlv_singleton_impl->delete_tag(btstack_tlv_singleton_context, tag);
}

void mesh_load_app_keys(void){
    printf("Load App Keys\n");
    uint16_t internal_index;
    for (internal_index = 0; internal_index < MAX_NR_MESH_TRANSPORT_KEYS; internal_index++){
        mesh_persistent_app_key_t data;
        uint32_t tag = mesh_transport_key_tag_for_internal_index(internal_index);
        int app_key_len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, tag, (uint8_t *) &data, sizeof(data));
        if (app_key_len == 0) continue;
        
        mesh_transport_key_t * key = btstack_memory_mesh_transport_key_get();
        if (key == NULL) return;

        key->internal_index = internal_index;
        key->appkey_index = data.appkey_index;
        key->netkey_index = data.netkey_index;
        key->aid          = data.aid;
        key->akf          = 1;
        key->version      = data.version;
        memcpy(key->key, data.key, 16);
        mesh_transport_key_add(key);
        printf("- internal index 0x%x, AppKey Index 0x%06x, AID %02x: ", key->internal_index, key->appkey_index, key->aid);
        printf_hexdump(key->key, 16);
    }
}

void mesh_delete_app_keys(void){
    printf("Delete App Keys\n");
    
    uint16_t internal_index;
    for (internal_index = 0; internal_index < MAX_NR_MESH_TRANSPORT_KEYS; internal_index++){
        mesh_delete_app_key(internal_index);
    }
}

static void mesh_node_setup_default_models(void){
    // configure Config Server
    mesh_configuration_server_model.model_identifier = mesh_model_get_model_identifier_bluetooth_sig(MESH_SIG_MODEL_ID_CONFIGURATION_SERVER);
    mesh_configuration_server_model.model_data       = &mesh_configuration_server_model_context;
    mesh_configuration_server_model.operations       = mesh_configuration_server_get_operations();    
    mesh_element_add_model(mesh_node_get_primary_element(), &mesh_configuration_server_model);

    // Config Health Server
    mesh_health_server_model.model_identifier = mesh_model_get_model_identifier_bluetooth_sig(MESH_SIG_MODEL_ID_HEALTH_SERVER);
    mesh_element_add_model(mesh_node_get_primary_element(), &mesh_health_server_model);
}

void mesh_init(void){

    // register for HCI events
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // ADV Bearer also used for GATT Proxy Advertisements and PB-GATT
    adv_bearer_init();

#ifdef ENABLE_MESH_GATT_BEARER
    // Setup GATT bearer
    gatt_bearer_init();
#endif

#ifdef ENABLE_MESH_ADV_BEARER
    // Setup Unprovisioned Device Beacon
    beacon_init();
#endif

    provisioning_device_init();

    // Node Configuration
    mesh_node_init();

    // Network layer
    mesh_network_init();

    // Transport layers (lower + upper))
    mesh_lower_transport_init();
    mesh_upper_transport_init();

    // Access layer
    mesh_access_init();

    mesh_node_setup_default_models();
}

/**
 * Register for Mesh Provisioning Device events
 * @param packet_handler
 */
void mesh_register_provisioning_device_packet_handler(btstack_packet_handler_t packet_handler){
    provisioning_device_packet_handler = packet_handler;
    provisioning_device_register_packet_handler(&mesh_provisioning_message_handler);
}