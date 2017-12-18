#include <dazzle.h>

static void
test_basic (void)
{
  g_autoptr(GCancellable) root = g_cancellable_new ();
  g_autoptr(GCancellable) a = g_cancellable_new ();
  g_autoptr(GCancellable) b = g_cancellable_new ();
  g_autoptr(GCancellable) a1 = g_cancellable_new ();
  g_autoptr(GCancellable) a2 = g_cancellable_new ();

  dzl_cancellable_chain (root, a);
  dzl_cancellable_chain (root, b);

  dzl_cancellable_chain (a, a1);
  dzl_cancellable_chain (a, a2);

  g_cancellable_cancel (a2);

  g_assert_cmpint (TRUE, ==, g_cancellable_is_cancelled (a2));
  g_assert_cmpint (TRUE, ==, g_cancellable_is_cancelled (a));
  g_assert_cmpint (TRUE, ==, g_cancellable_is_cancelled (root));

  g_assert_cmpint (FALSE, ==, g_cancellable_is_cancelled (a1));
  g_assert_cmpint (FALSE, ==, g_cancellable_is_cancelled (b));
}

gint
main (gint   argc,
      gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/Dazzle/Cancellable", test_basic);
  return g_test_run ();
}
