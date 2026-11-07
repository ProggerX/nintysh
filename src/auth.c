#include "auth.h"

#include "config.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *password;
    char *message;
} PamData;

static int pam_conversation(int count, const struct pam_message **messages,
                            struct pam_response **responses,
                            void *user_data) {
    PamData *data = user_data;
    if (count <= 0 || messages == NULL || responses == NULL) {
        return PAM_CONV_ERR;
    }

    struct pam_response *reply =
        calloc((size_t)count, sizeof(struct pam_response));
    if (reply == NULL) {
        return PAM_BUF_ERR;
    }

    for (int i = 0; i < count; i++) {
        switch (messages[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
            reply[i].resp = strdup(data->password != NULL ? data->password
                                                          : "");
            break;
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = strdup(g_get_user_name());
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            if (messages[i]->msg != NULL) {
                g_free(data->message);
                data->message = g_strdup(messages[i]->msg);
            }
            break;
        default:
            for (int j = 0; j < i; j++) {
                free(reply[j].resp);
            }
            free(reply);
            return PAM_CONV_ERR;
        }
    }

    *responses = reply;
    return PAM_SUCCESS;
}

gboolean ninty_auth_password(const char *password, char **message) {
    PamData data = {
        .password = password != NULL ? password : "",
        .message = NULL,
    };
    struct pam_conv conversation = {
        .conv = pam_conversation,
        .appdata_ptr = &data,
    };

    const char *username = g_get_user_name();
    if (username == NULL || username[0] == '\0') {
        struct passwd *entry = getpwuid(getuid());
        username = entry != NULL ? entry->pw_name : "";
    }

    pam_handle_t *handle = NULL;
    int result = pam_start(NINTYSH_PAM_SERVICE, username, &conversation,
                           &handle);
    if (result == PAM_SUCCESS) {
        result = pam_authenticate(handle, PAM_SILENT);
    }
    if (result == PAM_SUCCESS) {
        result = pam_acct_mgmt(handle, PAM_SILENT);
    }

    if (result != PAM_SUCCESS && message != NULL) {
        const char *error = handle != NULL
                                ? pam_strerror(handle, result)
                                : "Authentication failed";
        *message = g_strdup(data.message != NULL ? data.message : error);
    }

    if (handle != NULL) {
        pam_end(handle, result);
    }
    g_free(data.message);
    return result == PAM_SUCCESS;
}
