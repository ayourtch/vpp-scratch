/*
 * Copyright (c) 2017-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vnet/session/application_namespace.h>
#include <vnet/session/application.h>
#include <vnet/session/session_table.h>
#include <vnet/session/session.h>
#include <vnet/fib/fib_table.h>
#include <vppinfra/file.h>
#include <vlib/unix/unix.h>

/**
 * Hash table of application namespaces by app ns ids
 */
uword *app_namespace_lookup_table;

/**
 * Pool of application namespaces
 */
static app_namespace_t *app_namespace_pool;

static u8 app_sapi_enabled;

app_namespace_t *
app_namespace_get (u32 index)
{
  return pool_elt_at_index (app_namespace_pool, index);
}

app_namespace_t *
app_namespace_get_from_id (const u8 * ns_id)
{
  u32 index = app_namespace_index_from_id (ns_id);
  if (index == APP_NAMESPACE_INVALID_INDEX)
    return 0;
  return app_namespace_get (index);
}

u32
app_namespace_index (app_namespace_t * app_ns)
{
  return (app_ns - app_namespace_pool);
}

app_namespace_t *
app_namespace_alloc (u8 * ns_id)
{
  app_namespace_t *app_ns;
  pool_get (app_namespace_pool, app_ns);
  clib_memset (app_ns, 0, sizeof (*app_ns));
  app_ns->ns_id = vec_dup (ns_id);
  hash_set_mem (app_namespace_lookup_table, app_ns->ns_id,
		app_ns - app_namespace_pool);
  return app_ns;
}

int
vnet_app_namespace_add_del (vnet_app_namespace_add_del_args_t * a)
{
  app_namespace_t *app_ns;
  session_table_t *st;

  if (a->is_add)
    {
      if (a->sw_if_index != APP_NAMESPACE_INVALID_INDEX
	  && !vnet_get_sw_interface_or_null (vnet_get_main (),
					     a->sw_if_index))
	return VNET_API_ERROR_INVALID_SW_IF_INDEX;


      if (a->sw_if_index != APP_NAMESPACE_INVALID_INDEX)
	{
	  a->ip4_fib_id =
	    fib_table_get_table_id_for_sw_if_index (FIB_PROTOCOL_IP4,
						    a->sw_if_index);
	  a->ip6_fib_id =
	    fib_table_get_table_id_for_sw_if_index (FIB_PROTOCOL_IP6,
						    a->sw_if_index);
	}
      if (a->sw_if_index == APP_NAMESPACE_INVALID_INDEX
	  && a->ip4_fib_id == APP_NAMESPACE_INVALID_INDEX)
	return VNET_API_ERROR_INVALID_VALUE;

      app_ns = app_namespace_get_from_id (a->ns_id);
      if (!app_ns)
	{
	  app_ns = app_namespace_alloc (a->ns_id);
	  st = session_table_alloc ();
	  session_table_init (st, FIB_PROTOCOL_MAX);
	  st->is_local = 1;
	  st->appns_index = app_namespace_index (app_ns);
	  app_ns->local_table_index = session_table_index (st);
	}
      app_ns->ns_secret = a->secret;
      app_ns->netns = a->netns ? vec_dup (a->netns) : 0;
      app_ns->sw_if_index = a->sw_if_index;
      app_ns->ip4_fib_index =
	fib_table_find (FIB_PROTOCOL_IP4, a->ip4_fib_id);
      app_ns->ip6_fib_index =
	fib_table_find (FIB_PROTOCOL_IP6, a->ip6_fib_id);
      session_lookup_set_tables_appns (app_ns);

      /* Add socket for namespace */
      if (app_sapi_enabled)
	appns_sapi_add_ns_socket (app_ns);
    }
  else
    {
      return VNET_API_ERROR_UNIMPLEMENTED;
    }
  return 0;
}

const u8 *
app_namespace_id (app_namespace_t * app_ns)
{
  return app_ns->ns_id;
}

u32
app_namespace_index_from_id (const u8 * ns_id)
{
  uword *indexp;
  indexp = hash_get_mem (app_namespace_lookup_table, ns_id);
  if (!indexp)
    return APP_NAMESPACE_INVALID_INDEX;
  return *indexp;
}

const u8 *
app_namespace_id_from_index (u32 index)
{
  app_namespace_t *app_ns;

  app_ns = app_namespace_get (index);
  return app_namespace_id (app_ns);
}

u32
app_namespace_get_fib_index (app_namespace_t * app_ns, u8 fib_proto)
{
  return fib_proto == FIB_PROTOCOL_IP4 ?
    app_ns->ip4_fib_index : app_ns->ip6_fib_index;
}

session_table_t *
app_namespace_get_local_table (app_namespace_t * app_ns)
{
  return session_table_get (app_ns->local_table_index);
}

void
appns_sapi_enable (void)
{
  app_sapi_enabled = 1;
}

u8
appns_sapi_enabled (void)
{
  return app_sapi_enabled;
}

void
app_namespaces_init (void)
{
  u8 *ns_id = format (0, "default");

  if (!app_namespace_lookup_table)
    app_namespace_lookup_table =
      hash_create_vec (0, sizeof (u8), sizeof (uword));

  /*
   * Allocate default namespace
   */

  /* clang-format off */
  vnet_app_namespace_add_del_args_t a = {
    .ns_id = ns_id,
    .netns = 0,
    .secret = 0,
    .sw_if_index = APP_NAMESPACE_INVALID_INDEX,
    .is_add = 1
  };
  /* clang-format on */

  vnet_app_namespace_add_del (&a);
  vec_free (ns_id);
}

static clib_error_t *
app_ns_fn (vlib_main_t * vm, unformat_input_t * input,
	   vlib_cli_command_t * cmd)
{
  u8 is_add = 0, *ns_id = 0, secret_set = 0, sw_if_index_set = 0, *netns = 0;
  unformat_input_t _line_input, *line_input = &_line_input;
  u32 sw_if_index, fib_id = APP_NAMESPACE_INVALID_INDEX;
  u64 secret;
  clib_error_t *error = 0;
  int rv;

  session_cli_return_if_not_enabled ();

  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "add"))
	is_add = 1;
      else if (unformat (line_input, "id %_%v%_", &ns_id))
	;
      else if (unformat (line_input, "secret %lu", &secret))
	secret_set = 1;
      else if (unformat (line_input, "sw_if_index %u", &sw_if_index))
	sw_if_index_set = 1;
      else if (unformat (line_input, "fib_id", &fib_id))
	;
      else if (unformat (line_input, "netns %_%v%_", &netns))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (!ns_id || !secret_set || !sw_if_index_set)
    {
      vlib_cli_output (vm, "namespace-id, secret and sw_if_index must be "
		       "provided");
      goto done;
    }

  if (is_add)
    {
      /* clang-format off */
      vnet_app_namespace_add_del_args_t args = {
	.ns_id = ns_id,
	.netns = netns,
	.secret = secret,
	.sw_if_index = sw_if_index,
	.ip4_fib_id = fib_id,
	.is_add = 1
      };
      /* clang-format on */

      if ((rv = vnet_app_namespace_add_del (&args)))
	error = clib_error_return (0, "app namespace add del returned %d", rv);
    }

done:

  vec_free (ns_id);
  vec_free (netns);
  unformat_free (line_input);

  return error;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (app_ns_command, static) = {
  .path = "app ns",
  .short_help = "app ns [add] id <namespace-id> secret <secret> "
		"sw_if_index <sw_if_index> [netns <ns>]",
  .function = app_ns_fn,
};
/* *INDENT-ON* */

u8 *
format_app_namespace (u8 * s, va_list * args)
{
  app_namespace_t *app_ns = va_arg (*args, app_namespace_t *);

  s =
    format (s, "%-10u%-10lu%-15d%-15v%-15v%-40v", app_namespace_index (app_ns),
	    app_ns->ns_secret, app_ns->sw_if_index, app_ns->ns_id,
	    app_ns->netns, app_ns->sock_name);
  return s;
}

static void
app_namespace_show_api (vlib_main_t * vm, app_namespace_t * app_ns)
{
  app_ns_api_handle_t *handle;
  app_worker_t *app_wrk;
  clib_socket_t *cs;
  clib_file_t *cf;

  if (!app_sapi_enabled)
    {
      vlib_cli_output (vm, "app socket api not enabled!");
      return;
    }

  vlib_cli_output (vm, "socket: %v\n", app_ns->sock_name);

  if (!pool_elts (app_ns->app_sockets))
    return;

  vlib_cli_output (vm, "%12s%12s%5s", "app index", "wrk index", "fd");


  /* *INDENT-OFF* */
  pool_foreach (cs, app_ns->app_sockets)  {
    handle = (app_ns_api_handle_t *) &cs->private_data;
    cf = clib_file_get (&file_main, handle->aah_file_index);
    if (handle->aah_app_wrk_index == APP_INVALID_INDEX)
      {
	vlib_cli_output (vm, "%12d%12d%5u", -1, -1, cf->file_descriptor);
	continue;
      }
    app_wrk = app_worker_get (handle->aah_app_wrk_index);
    vlib_cli_output (vm, "%12d%12d%5u", app_wrk->app_index,
                     app_wrk->wrk_map_index, cf->file_descriptor);
  }
  /* *INDENT-ON* */
}

static clib_error_t *
show_app_ns_fn (vlib_main_t * vm, unformat_input_t * main_input,
		vlib_cli_command_t * cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  u8 *ns_id, do_table = 0, had_input = 1, do_api = 0;
  app_namespace_t *app_ns;
  session_table_t *st;

  session_cli_return_if_not_enabled ();

  if (!unformat_user (main_input, unformat_line_input, line_input))
    {
      had_input = 0;
      goto do_ns_list;
    }

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "table %_%v%_", &ns_id))
	do_table = 1;
      else if (unformat (line_input, "api-clients"))
	do_api = 1;
      else
	{
	  vlib_cli_output (vm, "unknown input [%U]", format_unformat_error,
			   line_input);
	  goto done;
	}
    }

  if (do_api)
    {
      if (!do_table)
	{
	  vlib_cli_output (vm, "must specify a table for api");
	  goto done;
	}
      app_ns = app_namespace_get_from_id (ns_id);
      app_namespace_show_api (vm, app_ns);
      goto done;
    }
  if (do_table)
    {
      app_ns = app_namespace_get_from_id (ns_id);
      if (!app_ns)
	{
	  vlib_cli_output (vm, "ns %v not found", ns_id);
	  goto done;
	}
      st = session_table_get (app_ns->local_table_index);
      if (!st)
	{
	  vlib_cli_output (vm, "table for ns %v could not be found", ns_id);
	  goto done;
	}
      session_lookup_show_table_entries (vm, st, 0, 1);
      vec_free (ns_id);
      goto done;
    }

do_ns_list:
  vlib_cli_output (vm, "%-10s%-10s%-15s%-15s%-15s%-40s", "Index", "Secret",
		   "sw_if_index", "Id", "netns", "Socket");

  /* *INDENT-OFF* */
  pool_foreach (app_ns, app_namespace_pool)  {
    vlib_cli_output (vm, "%U", format_app_namespace, app_ns);
  }
  /* *INDENT-ON* */

done:
  if (had_input)
    unformat_free (line_input);
  return 0;
}

/* *INDENT-OFF* */
VLIB_CLI_COMMAND (show_app_ns_command, static) =
{
  .path = "show app ns",
  .short_help = "show app ns [table <id> [api-clients]]",
  .function = show_app_ns_fn,
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
