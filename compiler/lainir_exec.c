#include "lainir_exec.h"

LainirExecStatus lainir_exec_request(
  sexp ctx,
  sexp env,
  const LainirExecRequest *request,
  sexp *result_out) {
  sexp proc =
    sexp_env_ref(ctx, env, sexp_intern(ctx, request->entry_name, -1), SEXP_FALSE);

  if (proc == SEXP_FALSE || !sexp_procedurep(proc)) {
    *result_out = sexp_user_exception(
      ctx,
      NULL,
      "lainir execution entry unavailable",
      sexp_c_string(ctx, request->entry_name, -1));
    return LAINIR_EXEC_UNAVAILABLE;
  }

  sexp result = sexp_apply(ctx, proc, request->args);
  *result_out = result;
  if (sexp_exceptionp(result))
    return LAINIR_EXEC_FAILED;

  return LAINIR_EXEC_OK;
}
