#define _ZS_PLUGIN__
#include "plg.h"
#include "string/xcpy.h"
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static void zsplg_setstdl(zsplg_handle_t *const handle, const zsplg_status st) {
  handle->st        = st;
  handle->error_str = dlerror();
}

zsplg_handle_t zsplg_open(const char * restrict file, const char * restrict modname, const bool do_strcpy) {
  /* load plugin with dlopen */
  zsplg_handle_t ret = {
    .st         = ZSP_OK,
    .modname    = 0,
    .mnlen      = strlen(modname),
    .dlh        = file ? dlopen(file, RTLD_LAZY | RTLD_LOCAL) : RTLD_DEFAULT,
    .have_alloc = do_strcpy,
  };
  if(zs_unlikely(file && !ret.dlh)) {
    zsplg_setstdl(&ret, ZSPE_DLOPN);
    return ret;
  }

  /* init_fn_name = "init_" + modname + "\0" */
  char init_fn_name[ret.mnlen + 6];
  memset(init_fn_name, 0, sizeof(init_fn_name));
  {
    char *tmp = init_fn_name;
    llzs_strixcpy(&tmp, "init_", 5);
    llzs_strixcpy(&tmp, modname, ret.mnlen);
  }

  /* initialize plugin */
  zsplugin_t* (*init_fn)() = dlsym(ret.dlh, init_fn_name);
  if(zs_likely(init_fn)) {
    ret.plugin  = *((*init_fn)());
    ret.modname = do_strcpy ? llzs_strxdup(modname, ret.mnlen) : modname;
  } else {
    zsplg_setstdl(&ret, ZSPE_DLOPN);
    /* cleanup dlh */
    if(file) {
      dlclose(ret.dlh);
      ret.dlh = 0;
    }
  }
  return ret;
}

bool zsplg_destroy(zsplg_gdsa_t *const gdsa) {
  if(zs_unlikely(!gdsa))
    return false;
  if(gdsa->destroy) {
    if(zs_unlikely(!gdsa->destroy(gdsa->data)))
      return false;
    gdsa->destroy = 0;
    gdsa->data = 0;
  }
  return true;
}

bool zsplg_close(zsplg_handle_t *const handle) {
  if(zs_unlikely(!handle))
    return false;

  zsplg_status st = handle->st;
  zsplugin_t *const plgptr = &handle->plugin;

  if(st == ZSPE_DLOPN)
    goto cont_nodlopen;

  if(zs_unlikely(!zsplg_destroy(&plgptr->data)))
    st = ZSPE_PLG;

  plgptr->data = ZS_GDSA(0, 0);

  /* unload plugin */
  if(!handle->dlh)
    goto cont_nodlopen;

  if(zs_likely(!dlclose(handle->dlh))) {
    handle->dlh = 0;
  } else {
    if(st == ZSP_OK) st = ZSPE_DLCLOS;
    handle->error_str = dlerror();
  }

 cont_nodlopen:
  if(handle->have_alloc) {
    free((void*)handle->modname);
    handle->modname = 0;
  }

  return ZSP_OK == (handle->st = st);
}

zsplg_gdsa_t zsplg_h_create(const zsplg_handle_t *const base, size_t argc, char *argv[]) {
  const zsplugin_t *const plgptr = &base->plugin;
  return plgptr->fn_h_create(plgptr->data.data, argc, argv);
}

static void zsplg_upd_errstr(zsplg_handle_t *const handle, const zsplg_status st) {
  const char *const tmp = dlerror();
  if(zs_likely(!tmp)) return;
  handle->error_str = tmp;
  if(st != ZSP_OK) handle->st = st;
}

zs_attrib(hot)
zsplg_gdsa_t zsplg_call_h(const zsplg_fncall_t *const fndat, void *const h_id) {
  /* handle error conditions */
  if(zs_unlikely(!fndat))
    RET_GDSA_NULL;

  zsplg_handle_t *const handle = fndat->plgh;
  const char     *const fn     = fndat->fn;

  if(zs_unlikely(!handle || !fn || handle->st == ZSPE_DLOPN))
    RET_GDSA_NULL;

  zsplg_gdsa_t (*xfn_ptr)(void *, size_t, char *const*);

  {
    /* construct function name */
    const size_t fnlen = strlen(fn);
    char xfn_name[handle->mnlen + fnlen + 3 + (h_id ? 1 : 0)];
    memset(xfn_name, 0, sizeof(xfn_name));
    {
      char *xnp = llzs_strxcpy(xfn_name, handle->modname, handle->mnlen);
               *(xnp++) = '_';
      if(h_id) *(xnp++) = 'h';
      llzs_strxcpy(stpcpy(xnp, "_"), fn, fnlen);
    }

    /* get function addr */
    zsplg_upd_errstr(handle, ZSP_OK);
    xfn_ptr = dlsym(handle->dlh, xfn_name);
  }

  if(zs_unlikely(!xfn_ptr)) {
    zsplg_upd_errstr(handle, ZSPE_DLSYM);
    RET_GDSA_NULL;
  }

  /* call function */
  return xfn_ptr(h_id ? h_id : handle->plugin.data.data, fndat->argc, fndat->argv);
}
