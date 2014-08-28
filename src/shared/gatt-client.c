/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "src/shared/att.h"
#include "src/shared/gatt-client.h"
#include "lib/uuid.h"
#include "src/shared/gatt-helpers.h"
#include "src/shared/util.h"

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define UUID_BYTES (BT_GATT_UUID_SIZE * sizeof(uint8_t))

struct service_list {
	bt_gatt_service_t service;
	struct service_list *next;
};

struct bt_gatt_client {
	struct bt_att *att;
	int ref_count;

	bt_gatt_client_callback_t ready_callback;
	bt_gatt_client_destroy_func_t ready_destroy;
	void *ready_data;

	bt_gatt_client_debug_func_t debug_callback;
	bt_gatt_client_destroy_func_t debug_destroy;
	void *debug_data;

	struct service_list *svc_head, *svc_tail;
	bool in_init;
	bool ready;
};

static bool gatt_client_add_service(struct bt_gatt_client *client,
						uint16_t start, uint16_t end,
						uint8_t uuid[BT_GATT_UUID_SIZE])
{
	struct service_list *list;

	list = new0(struct service_list, 1);
	if (!list)
		return false;

	list->service.start_handle = start;
	list->service.end_handle = end;
	memcpy(list->service.uuid, uuid, UUID_BYTES);

	if (!client->svc_head)
		client->svc_head = client->svc_tail = list;
	else {
		client->svc_tail->next = list;
		client->svc_tail = list;
	}

	return true;
}

static void service_destroy_characteristics(bt_gatt_service_t *service)
{
	unsigned int i;

	for (i = 0; i < service->num_chrcs; i++)
		free((bt_gatt_descriptor_t *) service->chrcs[i].descs);

	free((bt_gatt_characteristic_t *) service->chrcs);
}

static void gatt_client_clear_services(struct bt_gatt_client *client)
{
	struct service_list *l, *tmp;

	l = client->svc_head;

	while (l) {
		service_destroy_characteristics(&l->service);
		tmp = l;
		l = tmp->next;
		free(tmp);
	}

	client->svc_head = client->svc_tail = NULL;
}

struct async_op {
	struct bt_gatt_client *client;
	int ref_count;
};

static struct async_op *async_op_ref(struct async_op *op)
{
	__sync_fetch_and_add(&op->ref_count, 1);

	return op;
}

static void async_op_unref(void *data)
{
	struct async_op *op = data;

	if (__sync_sub_and_fetch(&op->ref_count, 1))
		return;

	free(data);
}

static void discover_primary_cb(bool success, uint8_t att_ecode,
						struct bt_gatt_result *result,
						void *user_data)
{
	struct async_op *op = user_data;
	struct bt_gatt_client *client = op->client;
	struct bt_gatt_iter iter;
	uint16_t start, end;
	uint8_t uuid[BT_GATT_UUID_SIZE];
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t tmp;

	if (!success) {
		util_debug(client->debug_callback, client->debug_data,
					"Primary service discovery failed."
					" ATT ECODE: 0x%02x", att_ecode);
		goto done;
	}

	if (!result || !bt_gatt_iter_init(&iter, result)) {
		success = false;
		att_ecode = 0;
		goto done;
	}

	util_debug(client->debug_callback, client->debug_data,
					"Primary services found: %u",
					bt_gatt_result_service_count(result));

	while (bt_gatt_iter_next_service(&iter, &start, &end, uuid)) {
		/* Log debug message. */
		tmp.type = BT_UUID128;
		memcpy(tmp.value.u128.data, uuid, sizeof(uuid));
		bt_uuid_to_string(&tmp, uuid_str, sizeof(uuid_str));
		util_debug(client->debug_callback, client->debug_data,
				"start: 0x%04x, end: 0x%04x, uuid: %s",
				start, end, uuid_str);

		/* Store the service */
		if (!gatt_client_add_service(client, start, end, uuid)) {
			util_debug(client->debug_callback, client->debug_data,
						"Failed to store service");

			gatt_client_clear_services(client);

			success = false;
			goto done;
		}
	}

done:
	client->in_init = false;

	if (success)
		client->ready = true;

	if (client->ready_callback)
		client->ready_callback(success, att_ecode, client->ready_data);
}

static void exchange_mtu_cb(bool success, uint8_t att_ecode, void *user_data)
{
	struct async_op *op = user_data;
	struct bt_gatt_client *client = op->client;

	if (!success) {
		util_debug(client->debug_callback, client->debug_data,
				"MTU Exchange failed. ATT ECODE: 0x%02x",
				att_ecode);

		client->in_init = false;

		if (client->ready_callback)
			client->ready_callback(success, att_ecode,
							client->ready_data);

		return;
	}

	util_debug(client->debug_callback, client->debug_data,
					"MTU exchange complete, with MTU: %u",
					bt_att_get_mtu(client->att));

	if (bt_gatt_discover_primary_services(client->att, NULL,
							discover_primary_cb,
							async_op_ref(op),
							async_op_unref))
		return;

	util_debug(client->debug_callback, client->debug_data,
			"Failed to initiate primary service discovery");

	client->in_init = false;

	if (client->ready_callback)
		client->ready_callback(success, att_ecode, client->ready_data);

	async_op_unref(op);
}

static bool gatt_client_init(struct bt_gatt_client *client, uint16_t mtu)
{
	struct async_op *op;

	if (client->in_init || client->ready)
		return false;

	op = new0(struct async_op, 1);
	if (!op)
		return false;

	op->client = client;

	/* Configure the MTU */
	if (!bt_gatt_exchange_mtu(client->att, MAX(BT_ATT_DEFAULT_LE_MTU, mtu),
							exchange_mtu_cb,
							async_op_ref(op),
							async_op_unref)) {
		if (client->ready_callback)
			client->ready_callback(false, 0, client->ready_data);

		free(op);
	}

	client->in_init = true;

	return true;
}

struct bt_gatt_client *bt_gatt_client_new(struct bt_att *att, uint16_t mtu)
{
	struct bt_gatt_client *client;

	if (!att)
		return NULL;

	client = new0(struct bt_gatt_client, 1);
	if (!client)
		return NULL;

	client->att = bt_att_ref(att);

	gatt_client_init(client, mtu);

	return bt_gatt_client_ref(client);
}

struct bt_gatt_client *bt_gatt_client_ref(struct bt_gatt_client *client)
{
	if (!client)
		return NULL;

	__sync_fetch_and_add(&client->ref_count, 1);

	return client;
}

void bt_gatt_client_unref(struct bt_gatt_client *client)
{
	if (!client)
		return;

	if (__sync_sub_and_fetch(&client->ref_count, 1))
		return;

	if (client->ready_destroy)
		client->ready_destroy(client->ready_data);

	if (client->debug_destroy)
		client->debug_destroy(client->debug_data);

	bt_att_unref(client->att);
	free(client);
}

bool bt_gatt_client_is_ready(struct bt_gatt_client *client)
{
	return (client && client->ready);
}

bool bt_gatt_client_set_ready_handler(struct bt_gatt_client *client,
					bt_gatt_client_callback_t callback,
					void *user_data,
					bt_gatt_client_destroy_func_t destroy)
{
	if (!client)
		return false;

	if (client->ready_destroy)
		client->ready_destroy(client->ready_data);

	client->ready_callback = callback;
	client->ready_destroy = destroy;
	client->ready_data = user_data;

	return true;
}

bool bt_gatt_client_set_debug(struct bt_gatt_client *client,
					bt_gatt_client_debug_func_t callback,
					void *user_data,
					bt_gatt_client_destroy_func_t destroy) {
	if (!client)
		return false;

	if (client->debug_destroy)
		client->debug_destroy(client->debug_data);

	client->debug_callback = callback;
	client->debug_destroy = destroy;
	client->debug_data = user_data;

	return true;
}

bool bt_gatt_service_iter_init(struct bt_gatt_service_iter *iter,
						struct bt_gatt_client *client)
{
	if (!iter || !client)
		return false;

	if (client->in_init)
		return false;

	memset(iter, 0, sizeof(*iter));
	iter->client = client;
	iter->ptr = NULL;

	return true;
}

bool bt_gatt_service_iter_next(struct bt_gatt_service_iter *iter,
						bt_gatt_service_t *service)
{
	struct service_list *l;

	if (!iter || !service)
		return false;

	l = iter->ptr;

	if (!l)
		l = iter->client->svc_head;
	else
		l = l->next;

	if (!l)
		return false;

	*service = l->service;
	iter->ptr = l;

	return true;
}

bool bt_gatt_service_iter_next_by_handle(struct bt_gatt_service_iter *iter,
						uint16_t start_handle,
						bt_gatt_service_t *service)
{
	while (bt_gatt_service_iter_next(iter, service)) {
		if (service->start_handle == start_handle)
			return true;
	}

	return false;
}

bool bt_gatt_service_iter_next_by_uuid(struct bt_gatt_service_iter *iter,
					const uint8_t uuid[BT_GATT_UUID_SIZE],
					bt_gatt_service_t *service)
{
	while (bt_gatt_service_iter_next(iter, service)) {
		if (memcmp(service->uuid, uuid, UUID_BYTES) == 0)
			return true;
	}

	return false;
}