#include "myssh.h"

ssh_session ssh_ptr_get(SEXP ptr){
  ssh_session ssh = (ssh_session) R_ExternalPtrAddr(ptr);
  if(ssh == NULL)
    Rf_error("SSH session pointer is dead");
  return ssh;
}

static void ssh_ptr_fin(SEXP ptr){
  ssh_session ssh = (ssh_session) R_ExternalPtrAddr(ptr);
  if(ssh == NULL)
    return;
  ssh_disconnect(ssh);
  ssh_free(ssh);
  R_ClearExternalPtr(ptr);
}

static SEXP ssh_ptr_create(ssh_session ssh){
  SEXP ptr = PROTECT(R_MakeExternalPtr(ssh, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ptr, ssh_ptr_fin, TRUE);
  Rf_setAttrib(ptr, R_ClassSymbol, Rf_mkString("ssh_session"));
  UNPROTECT(1);
  return ptr;
}

void bail_if(int rc, const char * what, ssh_session ssh){
  if (rc != SSH_OK){
    char buf[1024];
    strncpy(buf, ssh_get_error(ssh), 1024);
    ssh_disconnect(ssh);
    ssh_free(ssh);
    Rf_errorcall(R_NilValue, "libssh failure at '%s': %s", what, buf);
  }
}

static size_t password_cb(SEXP rpass, const char * prompt, char buf[1024]){
  if(Rf_isString(rpass) && Rf_length(rpass)){
    strncpy(buf, CHAR(STRING_ELT(rpass, 0)), 1024);
    return Rf_length(STRING_ELT(rpass, 0));
  } else if(Rf_isFunction(rpass)){
    int err;
    SEXP call = PROTECT(Rf_lcons(rpass, Rf_lcons(make_string(prompt), R_NilValue)));
    SEXP res = PROTECT(R_tryEval(call, R_GlobalEnv, &err));
    if(err || !Rf_isString(res)){
      UNPROTECT(2);
      Rf_error("Password callback did not return a string value");
    }
    strncpy(buf, CHAR(STRING_ELT(res, 0)), 1024);
    UNPROTECT(2);
    return strlen(buf);
  }
  Rf_errorcall(R_NilValue, "unsupported password type");
}

int my_auth_callback(const char *prompt, char *buf, size_t len, int echo, int verify, void *userdata){
  Rprintf("Calling my_auth_callback\n");
  SEXP rpass = (SEXP) userdata;
  password_cb(rpass, prompt, buf);
  return SSH_OK;
}

static int auth_password(ssh_session ssh, SEXP rpass){
  char buf[1024];
  password_cb(rpass, "Please enter your password", buf);
  int rc = ssh_userauth_password(ssh, NULL, buf);
  bail_if(rc == SSH_AUTH_ERROR, "password auth", ssh);
  return rc;
}

static int auth_interactive(ssh_session ssh, SEXP rpass){
  int rc = ssh_userauth_kbdint(ssh, NULL, NULL);
  while (rc == SSH_AUTH_INFO) {
    const char * name = ssh_userauth_kbdint_getname(ssh);
    const char * instruction = ssh_userauth_kbdint_getinstruction(ssh);
    int nprompts = ssh_userauth_kbdint_getnprompts(ssh);
    if (strlen(name) > 0)
      Rprintf("%s\n", name);
    if (strlen(instruction) > 0)
      Rprintf("%s\n", instruction);
    for (int iprompt = 0; iprompt < nprompts; iprompt++) {
      char buf[1024];
      const char * prompt = ssh_userauth_kbdint_getprompt(ssh, iprompt, NULL);
      password_cb(rpass, prompt, buf);
      if (ssh_userauth_kbdint_setanswer(ssh, iprompt, buf) < 0)
        return SSH_AUTH_ERROR;
    }
    rc = ssh_userauth_kbdint(ssh, NULL, NULL);
  }
  return rc;
}

/* authenticate client */
static void auth_any(ssh_session ssh, ssh_key privkey, SEXP rpass){
  if(ssh_userauth_none(ssh, NULL) == SSH_AUTH_SUCCESS)
    return;
  int method = ssh_userauth_list(ssh, NULL);
  if (method & SSH_AUTH_METHOD_PUBLICKEY){
    if(privkey != NULL && ssh_userauth_publickey(ssh, NULL, privkey) == SSH_AUTH_SUCCESS)
      return;
    if(privkey == NULL && ssh_userauth_publickey_auto(ssh, NULL, NULL) == SSH_AUTH_SUCCESS)
      return;
  }
  if (method & SSH_AUTH_METHOD_INTERACTIVE && auth_interactive(ssh, rpass) == SSH_AUTH_SUCCESS)
    return;
  if (method & SSH_AUTH_METHOD_PASSWORD && auth_password(ssh, rpass) == SSH_AUTH_SUCCESS)
    return;
  Rf_error("Authentication failed, permission denied");
}

SEXP C_start_session(SEXP rhost, SEXP rport, SEXP ruser, SEXP keyfile, SEXP rpass){

  /* try reading private key first */
  ssh_key privkey = NULL;
  if(Rf_length(keyfile))
    if(ssh_pki_import_privkey_file(CHAR(STRING_ELT(keyfile, 0)), NULL, my_auth_callback, rpass, &privkey) != SSH_OK)
      Rf_error("Failed to read private key: %s", CHAR(STRING_ELT(keyfile, 0)));

  /* load options */
  int port = Rf_asInteger(rport);
  const char * host = CHAR(STRING_ELT(rhost, 0));
  const char * user = CHAR(STRING_ELT(ruser, 0));
  ssh_session ssh = ssh_new();
  bail_if(ssh_options_set(ssh, SSH_OPTIONS_HOST, host), "set host", ssh);
  bail_if(ssh_options_set(ssh, SSH_OPTIONS_USER, user), "set user", ssh);
  bail_if(ssh_options_set(ssh, SSH_OPTIONS_PORT, &port), "set port", ssh);

  /* sets password callback for default private key */
  struct ssh_callbacks_struct cb = {
    .userdata = rpass,
    .auth_function = my_auth_callback
  };
  ssh_callbacks_init(&cb);
  ssh_set_callbacks(ssh, &cb);

  /* connect */
  bail_if(ssh_connect(ssh), "connect", ssh);

  /* get server identity */
  ssh_key key;
  unsigned char * hash = NULL;
  size_t hlen = 0;
  ssh_get_publickey(ssh, &key);
  ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA1, &hash, &hlen);
  if(!ssh_is_server_known(ssh))
    Rprintf("Unknown server fingerprint: %s\n", ssh_get_hexa(hash, hlen));

  /* Authenticate client */
  auth_any(ssh, privkey, rpass);

  /* display welcome message */
  char * banner = ssh_get_issue_banner(ssh);
  if(banner != NULL){
    Rprintf("%s\n", banner);
    free(banner);
  }

  return ssh_ptr_create(ssh);
}

SEXP C_disconnect_session(SEXP ptr){
  ssh_ptr_fin(ptr);
  return R_NilValue;
}
