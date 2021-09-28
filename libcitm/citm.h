derr_t citm(
   const char *local_host,
   const char *local_svc,
   const char *key,
   const char *cert,
   const char *dh,
   const char *remote_host,
   const char *remote_svc,
   const string_builder_t *maildir_root,
   bool indicate_ready
);

// this is exposed so that the windows service-handler can call it manually
void stop_loop_on_signal(int signum);
