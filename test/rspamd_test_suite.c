#include "../src/config.h"
#include "../src/main.h"
#include "../src/cfg_file.h"
#include "tests.h"

static gboolean do_debug;
struct rspamd_main             *rspamd_main = NULL;

static GOptionEntry entries[] =
{
  { "debug", 'd', 0, G_OPTION_ARG_NONE, &do_debug, "Turn on debug messages", NULL },
  { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
};

int
main (int argc, char **argv)
{
	struct config_file             *cfg;
	GError                         *error = NULL;
	GOptionContext                 *context;

	context = g_option_context_new ("- run rspamd test suite");
	g_option_context_set_summary (context, "Summary:\n  Rspamd test suite version " RVERSION);
	g_option_context_add_main_entries (context, entries, NULL);
	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		fprintf (stderr, "option parsing failed: %s\n", error->message);
		exit (1);
	}

	g_mem_set_vtable(glib_mem_profiler_table);

	g_test_init (&argc, &argv, NULL);

	rspamd_main = (struct rspamd_main *)g_malloc (sizeof (struct rspamd_main));
	memset (rspamd_main, 0, sizeof (struct rspamd_main));
	rspamd_main->server_pool = memory_pool_new (memory_pool_get_size ());
	rspamd_main->cfg = (struct config_file *)g_malloc (sizeof (struct config_file));
	cfg = rspamd_main->cfg;
	bzero (cfg, sizeof (struct config_file));
	cfg->cfg_pool = memory_pool_new (memory_pool_get_size ());

	if (do_debug) {
		cfg->log_level = G_LOG_LEVEL_DEBUG;
	}
	else {
		cfg->log_level = G_LOG_LEVEL_INFO;
	}
	/* First set logger to console logger */
	rspamd_set_logger (RSPAMD_LOG_CONSOLE, TYPE_MAIN, rspamd_main);
	(void)open_log (rspamd_main->logger);
	g_log_set_default_handler (rspamd_glib_log_function, rspamd_main->logger);

	g_test_add_func ("/rspamd/memcached", rspamd_memcached_test_func);
	g_test_add_func ("/rspamd/mem_pool", rspamd_mem_pool_test_func);
	g_test_add_func ("/rspamd/fuzzy", rspamd_fuzzy_test_func);
	g_test_add_func ("/rspamd/url", rspamd_url_test_func);
	g_test_add_func ("/rspamd/expression", rspamd_expression_test_func);
	g_test_add_func ("/rspamd/statfile", rspamd_statfile_test_func);
	g_test_add_func ("/rspamd/dns", rspamd_dns_test_func);

	g_test_run ();

	return 0;
}
