/* security: the part of macOS's security(1) that Claude Code actually calls,
 * backed by the iOS keychain.
 *
 * Claude Code keeps credentials in the keychain by shelling out to `security`.
 * iOS has no such tool, so every write failed; worse, the failure looks like a
 * failed *read* to the caller, which classes it transient and therefore never
 * falls back to its plaintext file. That is the whole reason `claude auth
 * login` signs in and then forgets it. Putting this on PATH ahead of the binary
 * makes the native sign-in, refresh and logout work as they do on a Mac.
 *
 * The four forms it uses, all on generic passwords:
 *
 *   security find-generic-password -a <account> -w -s <service>
 *   security add-generic-password -U -a <account> -s <service> -X <hex>
 *   security delete-generic-password -a <account> -s <service>
 *   security -i          (the add above, quoted, on stdin)
 *
 * Exit codes follow security(1) where it matters: 44 for "not found", which the
 * caller treats as "no credential yet" rather than as a broken keychain, and
 * the stderr text carries "could not be found in the keychain", which its
 * logout path greps for. Anything else non-zero means a real failure.
 *
 * Authorization comes from the signature, exactly as for ccauth-keychain: ldid
 * with the package's entitlements.plist, whose keychain-access-groups entry is
 * what makes SecItemAdd return 0 instead of -34018. Items are stored
 * ThisDeviceOnly and after-first-unlock, so they are excluded from backups and
 * survive reboots without the device being unlocked first.
 *
 * Deliberately NOT a general security(1): no certificates, no keychain
 * management, no interactive prompting. Unknown subcommands say so and exit 2.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXIT_NOT_FOUND 44
#define EXIT_DUPLICATE 45
#define NOT_FOUND_TEXT "The specified item could not be found in the keychain."

static CFStringRef cfstr(const char *s) {
    return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8);
}

static void put_str(CFMutableDictionaryRef d, CFTypeRef key, const char *val) {
    if (val == NULL || *val == '\0') return;
    CFStringRef v = cfstr(val);
    if (v != NULL) {
        CFDictionarySetValue(d, key, v);
        CFRelease(v);
    }
}

static CFMutableDictionaryRef query_for(const char *service, const char *account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    put_str(q, kSecAttrService, service);
    put_str(q, kSecAttrAccount, account);
    return q;
}

static int not_found(const char *who) {
    fprintf(stderr, "security: %s: %s\n", who, NOT_FOUND_TEXT);
    return EXIT_NOT_FOUND;
}

static int failed(const char *what, OSStatus st) {
    if (st == errSecMissingEntitlement)
        fprintf(stderr,
                "security: %s: -34018 (errSecMissingEntitlement) -- this binary is not "
                "signed with the package's entitlements.plist; reinstall "
                "com.andi.claude-code-native\n", what);
    else
        fprintf(stderr, "security: %s: OSStatus %d\n", what, (int)st);
    return 1;
}

/* --- options ----------------------------------------------------------- */

/* security(1) spells the same flag differently per subcommand -- -w is a
 * boolean for find and takes the password for add -- so each caller says which
 * letters take a value and everything else is a flag. Unknown flags are
 * ignored rather than fatal: -T, -A and friends only matter on a Mac. */
typedef struct {
    const char *service, *account, *password, *hex, *label;
    int update, want_password;
} opts;

static int parse(int argc, char **argv, const char *takes_value, opts *o) {
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') continue;
        char f = a[1];
        int has_value = strchr(takes_value, f) != NULL;
        const char *value = NULL;
        if (has_value) {
            if (a[2] != '\0') {
                value = a + 2;                      /* -sService */
            } else if (i + 1 < argc) {
                value = argv[++i];                  /* -s Service */
            } else {
                fprintf(stderr, "security: option -%c needs a value\n", f);
                return -1;
            }
        }
        switch (f) {
        case 'a': o->account = value; break;
        case 's': o->service = value; break;
        case 'l': o->label = value; break;
        case 'X': o->hex = value; break;
        case 'U': o->update = 1; break;
        case 'w':
            if (has_value) o->password = value;
            else o->want_password = 1;
            break;
        default: break;                             /* -g, -T, -A, -j, ... */
        }
    }
    return 0;
}

static CFDataRef data_from(const opts *o) {
    if (o->hex != NULL) {
        size_t n = strlen(o->hex);
        if (n % 2 != 0) {
            fprintf(stderr, "security: -X value is not hex\n");
            return NULL;
        }
        UInt8 *buf = malloc(n / 2 ? n / 2 : 1);
        if (buf == NULL) return NULL;
        for (size_t i = 0; i < n; i += 2) {
            unsigned byte;
            char pair[3] = { o->hex[i], o->hex[i + 1], '\0' };
            if (sscanf(pair, "%2x", &byte) != 1) {
                fprintf(stderr, "security: -X value is not hex\n");
                free(buf);
                return NULL;
            }
            buf[i / 2] = (UInt8)byte;
        }
        CFDataRef d = CFDataCreate(NULL, buf, (CFIndex)(n / 2));
        free(buf);
        return d;
    }
    if (o->password != NULL)
        return CFDataCreate(NULL, (const UInt8 *)o->password, (CFIndex)strlen(o->password));
    fprintf(stderr, "security: no password given (-w or -X)\n");
    return NULL;
}

/* --- subcommands ------------------------------------------------------- */

static int cmd_find(int argc, char **argv) {
    opts o = { 0 };
    if (parse(argc, argv, "acCDGjlst", &o) != 0) return 1;

    CFMutableDictionaryRef q = query_for(o.service, o.account);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    if (o.want_password) CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    else                 CFDictionarySetValue(q, kSecReturnAttributes, kCFBooleanTrue);

    CFTypeRef out = NULL;
    OSStatus st = SecItemCopyMatching(q, &out);
    CFRelease(q);
    if (st == errSecItemNotFound) return not_found("SecKeychainSearchCopyNext");
    if (st != errSecSuccess) return failed("find-generic-password", st);

    if (o.want_password && out != NULL) {
        CFDataRef d = (CFDataRef)out;
        fwrite(CFDataGetBytePtr(d), 1, (size_t)CFDataGetLength(d), stdout);
        fputc('\n', stdout);
    } else {
        /* Attribute dump is only ever read by a human here. */
        printf("keychain: \"iOS\"\nclass: \"genp\"\nattributes:\n");
        if (o.service) printf("    \"svce\"<blob>=\"%s\"\n", o.service);
        if (o.account) printf("    \"acct\"<blob>=\"%s\"\n", o.account);
    }
    if (out != NULL) CFRelease(out);
    return 0;
}

static int cmd_add(int argc, char **argv) {
    opts o = { 0 };
    if (parse(argc, argv, "acCDGjlsTwX", &o) != 0) return 1;

    CFDataRef data = data_from(&o);
    if (data == NULL) return 1;

    CFMutableDictionaryRef item = query_for(o.service, o.account);
    put_str(item, kSecAttrLabel, o.label != NULL ? o.label : o.service);
    CFDictionarySetValue(item, kSecValueData, data);
    CFDictionarySetValue(item, kSecAttrAccessible,
                         kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    OSStatus st = SecItemAdd(item, NULL);
    if (st == errSecDuplicateItem) {
        if (!o.update) {
            CFRelease(item);
            CFRelease(data);
            fprintf(stderr, "security: SecKeychainItemCreateFromContent: "
                            "The specified item already exists in the keychain.\n");
            return EXIT_DUPLICATE;
        }
        CFMutableDictionaryRef q = query_for(o.service, o.account);
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attrs, kSecValueData, data);
        CFDictionarySetValue(attrs, kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        st = SecItemUpdate(q, attrs);
        CFRelease(q);
        CFRelease(attrs);
    }
    CFRelease(item);
    CFRelease(data);
    if (st != errSecSuccess) return failed("add-generic-password", st);
    return 0;
}

static int cmd_delete(int argc, char **argv) {
    opts o = { 0 };
    if (parse(argc, argv, "acCDGjls", &o) != 0) return 1;

    CFMutableDictionaryRef q = query_for(o.service, o.account);
    OSStatus st = SecItemDelete(q);
    CFRelease(q);
    if (st == errSecItemNotFound) return not_found("SecKeychainSearchCopyNext");
    if (st != errSecSuccess) return failed("delete-generic-password", st);
    return 0;
}

static int dispatch(int argc, char **argv);

/* --- `security -i`: the same commands, quoted, on stdin ----------------- */

static int run_line(char *line) {
    char *argv[64];
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        char quote = '\0';
        if (*p == '"' || *p == '\'') quote = *p++;
        char *out = p, *start = p;
        while (*p != '\0') {
            if (quote != '\0') {
                if (*p == quote) { p++; break; }
            } else if (*p == ' ' || *p == '\t') {
                break;
            }
            if (*p == '\\' && p[1] != '\0') p++;
            *out++ = *p++;
        }
        if (*p != '\0') p++;
        *out = '\0';
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    if (argc == 0) return 0;
    if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) return 0;
    return dispatch(argc, argv);
}

static int cmd_stdin(void) {
    char line[65536];
    int worst = 0;
    while (fgets(line, sizeof line, stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int rc = run_line(line);
        if (rc != 0) worst = rc;
    }
    return worst;
}

static int dispatch(int argc, char **argv) {
    const char *cmd = argv[0];
    if (strcmp(cmd, "find-generic-password") == 0)   return cmd_find(argc - 1, argv + 1);
    if (strcmp(cmd, "add-generic-password") == 0)    return cmd_add(argc - 1, argv + 1);
    if (strcmp(cmd, "delete-generic-password") == 0) return cmd_delete(argc - 1, argv + 1);
    if (strcmp(cmd, "show-keychain-info") == 0) {
        fprintf(stderr, "Keychain \"iOS\" lock-on-sleep timeout=0\n");
        return 0;
    }
    if (strcmp(cmd, "-i") == 0) return cmd_stdin();
    fprintf(stderr, "security: unknown command \"%s\" -- this is CCForiOS's stand-in for "
                    "macOS security(1), which implements only generic-password access.\n", cmd);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: security [-i] <find|add|delete>-generic-password [options]\n");
        return 2;
    }
    return dispatch(argc - 1, argv + 1);
}
