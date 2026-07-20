/* wisp-lock-helper — minimal PAM auth wrapper, the only binary in dwlarp
 * that links against libpam. Reads a single attempt from stdin and replies
 * one line on stdout. Lifecycle is one process per attempt, spawned and
 * pipe-talked-to from lock.c.
 *
 * Protocol (line-based, both directions):
 *     stdin:  <password>\n          (no embedded newlines)
 *     stdout: ok\n    or   fail\n
 *
 * Exit status mirrors stdout for the caller's convenience but the daemon
 * uses the line. */
#define _GNU_SOURCE
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Captured by the conversation function via appdata_ptr. */
struct conv_ctx {
    const char *password;
    int         used;
};

static int conv_fn(int num_msg, const struct pam_message **msg,
                   struct pam_response **resp, void *appdata) {
    struct conv_ctx *ctx = appdata;
    if (num_msg <= 0) return PAM_CONV_ERR;
    struct pam_response *r = calloc(num_msg, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            r[i].resp = strdup(ctx->used ? "" : ctx->password);
            if (!r[i].resp) { free(r); return PAM_BUF_ERR; }
            ctx->used = 1;
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            r[i].resp = NULL;
            break;
        default:
            free(r);
            return PAM_CONV_ERR;
        }
    }
    *resp = r;
    return PAM_SUCCESS;
}

int main(int argc, char **argv) {
    const char *service = argc > 1 ? argv[1] : "system-auth";

    char pw[1024];
    ssize_t n = read(0, pw, sizeof pw - 1);
    if (n <= 0) { fputs("fail\n", stdout); return 1; }
    pw[n] = 0;
    char *nl = strchr(pw, '\n'); if (nl) *nl = 0;

    struct passwd *pwd = getpwuid(getuid());
    if (!pwd) { explicit_bzero(pw, sizeof pw); fputs("fail\n", stdout); return 1; }

    struct conv_ctx ctx = { .password = pw, .used = 0 };
    struct pam_conv conv = { .conv = conv_fn, .appdata_ptr = &ctx };
    pam_handle_t *ph = NULL;
    int rc = pam_start(service, pwd->pw_name, &conv, &ph);
    if (rc != PAM_SUCCESS) {
        explicit_bzero(pw, sizeof pw);
        fputs("fail\n", stdout); return 1;
    }
    rc = pam_authenticate(ph, 0);
    if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(ph, 0);
    pam_end(ph, rc);

    explicit_bzero(pw, sizeof pw);
    if (rc == PAM_SUCCESS) { fputs("ok\n",   stdout); return 0; }
    else                   { fputs("fail\n", stdout); return 1; }
}
