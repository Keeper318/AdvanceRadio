#include <curl/curl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define scrBegin         static int scrLine = 0; switch (scrLine) { case 0:
#define scrFinish(z)     default:; } scrLine = 0; return z
#define scrReturn(z)     \
        do {\
            scrLine = __LINE__;\
            return z; case __LINE__:;\
        } while (0)
#define NO_FILE stdin

enum {
    OGG_HEADER_SIZE = 27,
    OGG_HEADER_TYPE_FLAG = 5,
    OGG_FLAG_CONTINUED_PACKET = 0x01,
    OGG_FLAG_BOS = 0x02
};

enum {
    VORBIS_ID_HEADER = 1,
    VORBIS_COMMENT_HEADER = 3,
    VORBIS_SETUP_HEADER = 5
};

enum {
    CHANGE_SAVE_DESTINATION = -1,
    NAME_PLACEHOLDER = '_'
};

const char *const VORBIS_PATTERN = "vorbis";
const char *const ARTIST_FIELD = "ARTIST";
const char *const TITLE_FIELD = "TITLE";
const char *const ICECAST_ENCODER_FIELD = "ENCODER";
const char *const TITLE_UNKNOWN = "Unknown";
const char *const NO_TITLE = "unknown";
const char *const OGG_EXTENSION = ".ogg";
const char *const INVALID_FILENAME_CHARS = "\"*/:<>?\\|";

const char *radio_name;
static volatile sig_atomic_t interrupt = false;

void signal_handler(__attribute__((unused)) int _) {
    interrupt = true;
}

void error(const char *message) {
    fputs(message, stderr);
    putc('\n', stderr);
    interrupt = true;
}

unsigned read_uint(const uint8_t **data) {
    return *(*(const unsigned **) data)++;
}

const char *read_str(const uint8_t **data, size_t length) {
    const char **cdata = (const char **) data;
    const char *str = *cdata;
    *cdata += length;
    return str;
}

const char *
get_filename(const char *artist, unsigned artist_length, const char *title,
             unsigned title_length) {
    static char buf[UINT8_MAX + 1];
    if (!title) {
        title = NO_TITLE;
        title_length = strlen(NO_TITLE);
    }
    int length = snprintf(buf, UINT8_MAX + 1, "%.*s%s%.*s - ",
                          artist_length, artist ? artist : "",
                          artist ? " - " : "", title_length, title);
    time_t raw_time = time(NULL);
    struct tm *local_time = localtime(&raw_time);
    length += (int) strftime(buf + length, UINT8_MAX + 1 - length, "%F %T",
                             local_time);
    int max_length = UINT8_MAX - strlen(OGG_EXTENSION);
    length = length < max_length ? length : max_length;
    strcpy(buf + length, OGG_EXTENSION);
    length += strlen(OGG_EXTENSION);
    for (char *ptr = strchr(buf, '\0'); ptr < buf + length;) {
        *ptr = NAME_PLACEHOLDER;
        ptr = strchr(ptr, '\0');
    }
    for (char *ptr = strpbrk(buf, INVALID_FILENAME_CHARS); ptr;) {
        *ptr = NAME_PLACEHOLDER;
        ptr = strpbrk(ptr, INVALID_FILENAME_CHARS);
    }
    return buf;
}

// (?:ARTIST=.*\n)?TITLE=.*\n(?:ALBUM=.*\n)?(?:GENRE=.*\n)?(?:DATE=\d{4}\n)?(?:COMMENT=.*\n)?ENCODER=Liquidsoap/1\.1\.1 \(Unix; OCaml 4\.01\.0\)
const char *vorbis_parse_comments(const uint8_t *header) {
    const char *artist = NULL, *title = NULL;
    unsigned artist_length = 0, title_length = 0;
    unsigned comment_count = read_uint(&header);
    for (int i = 0; i < comment_count; ++i) {
        unsigned comment_length = read_uint(&header);
        const char *comment = read_str(&header, comment_length);
        const char *name = comment;
        const char *value = memchr(comment, '=', comment_length) + 1;
        unsigned name_length = value - 1 - name;
        unsigned value_length = comment_length - name_length - 1;
        if (memcmp(name, ARTIST_FIELD, name_length) == 0) {
            artist = value;
            artist_length = value_length;
        } else if (memcmp(name, TITLE_FIELD, name_length) == 0) {
            if (memcmp(value, TITLE_UNKNOWN, value_length) != 0 &&
                memcmp(value, radio_name, value_length) != 0) {
                title = value;
                title_length = value_length;
            }
        } else if (memcmp(name, ICECAST_ENCODER_FIELD, name_length) == 0) {
            continue;
        }
        printf("%.*s\n", comment_length, comment);
    }
    return get_filename(artist, artist_length, title, title_length);
}

const char *vorbis_comment_header(const uint8_t *header) {
    time_t raw_time = time(NULL);
    printf("\n%s", ctime(&raw_time));
    read_str(&header, strlen(VORBIS_PATTERN));
    unsigned vendor_length = read_uint(&header);
    read_str(&header, vendor_length);
    return vorbis_parse_comments(header);
}

const char *
vorbis_packet(const uint8_t *const segment, uint8_t length, bool new_packet) {
    static const uint8_t *packet = NULL;
    static uint8_t type;
    if (!packet) {
        if (!new_packet || (*segment & 1) == 0) {
            return NULL;
        }
        packet = segment;
        type = *packet++;
    }
    if (length == UINT8_MAX) {
        return NULL;
    }
    const char *result = NULL;
    switch (type) {
    case VORBIS_ID_HEADER:
        break;
    case VORBIS_COMMENT_HEADER:
        result = vorbis_comment_header(packet);
        break;
    case VORBIS_SETUP_HEADER:
        break;
    default:
        error("Invalid packet");
    }
    packet = NULL;
    return result;
}

FILE *try_open(char *pathname, const char *mode) {
    FILE *file;
    for (int attempt = 0; attempt < 3; ++attempt) {
        switch (attempt) {
        case 1:
            for (char *ptr = pathname; *ptr; ++ptr) {
                if ((signed char) *ptr < 0) {
                    *ptr = NAME_PLACEHOLDER;
                }
            }
            break;
        case 2:
            get_filename(NULL, 0, NULL, 0);
            break;
        default:
            break;
        }
        if ((file = fopen(pathname, mode))) {
            if (attempt > 0) {
                printf("Alternative filename: %s\n", pathname);
            }
            break;
        }
        perror(pathname);
    }
    return file;
}

const void *save(const void *, ssize_t, ...);

const char *change_save_destination( // NOLINT(misc-no-recursion)
        char *new_filename, FILE **file, void **buf, size_t *buf_size) {
    static const char *filename = NULL;
    const char *old_filename = filename;
    if (*file && *file != NO_FILE) {
        fclose(*file);
    }
    *file = NULL;
    filename = NULL;
    if (new_filename) {
        if ((*file = try_open(new_filename, "wb"))) {
            filename = new_filename;
        } else {
            error("");
            *file = NO_FILE;
        }
        save(*buf, (ssize_t) *buf_size);
    }
    free(*buf);
    *buf = NULL;
    *buf_size = 0;
    return old_filename;
}

const void *save(const void *ptr, ssize_t size, // NOLINT(misc-no-recursion)
                 ...) {
    static FILE *file = NULL;
    static void *buf = NULL;
    static size_t buf_size = 0;
    if (size == CHANGE_SAVE_DESTINATION) {
        va_list args;
        va_start(args, size);
        char *filename = va_arg(args, char *);
        va_end(args);
        return change_save_destination(filename, &file, &buf, &buf_size);
    }
    if (file) {
        if (ptr && file != NO_FILE && !fwrite(ptr, size, 1, file)) {
            perror("Cannot write to the file"); // Success?!
            error("");
        }
        return NULL;
    }
    size_t new_buf_size = buf_size + size;
    buf = realloc(buf, new_buf_size);
    if (!buf) {
        error("Out of memory");
        return NULL;
    }
    void *buf_added = buf + buf_size;
    buf_size = new_buf_size;
    memcpy(buf_added, ptr, size);
    return buf_added;
}

void
ogg_segment(const void *data, size_t *current_packet_size, uint8_t lacing) {
    const void *saved_data = save(data, lacing);
    const char *filename = vorbis_packet(saved_data ? saved_data : data, lacing,
                                         *current_packet_size == 0);
    if (filename) {
        save(NULL, CHANGE_SAVE_DESTINATION, filename);
    }
    *current_packet_size += lacing;
    if (lacing < UINT8_MAX) {
        *current_packet_size = 0;
    }
}

void ogg_track_end(bool complete) {
    const char *filename = save(NULL, CHANGE_SAVE_DESTINATION, NULL);
    if (filename && !complete && remove(filename) < 0) {
        perror(filename);
        error("");
    }
}

int scr__ogg_page(const void *data) {
    static size_t current_packet_size = 0;
    static uint8_t page_segments;
    static uint8_t segment_table[UINT8_MAX];
    static uint8_t *lacing;
    static int track_counter = -1;
    scrBegin;
        scrReturn(interrupt ? -1 : OGG_HEADER_SIZE);
        const uint8_t *header = data;
        uint8_t flag = header[OGG_HEADER_TYPE_FLAG];
        if (current_packet_size ^ (flag & OGG_FLAG_CONTINUED_PACKET)) {
            error("Invalid page");
        }
        if (flag & OGG_FLAG_BOS) {
            ogg_track_end(track_counter++ > 0);
        }
        save(header, OGG_HEADER_SIZE);
        page_segments = header[OGG_HEADER_SIZE - 1];
        scrReturn(page_segments);
        memcpy(segment_table, data, page_segments);
        save(segment_table, page_segments);
        for (lacing = segment_table;
                lacing < segment_table + page_segments; ++lacing) {
            scrReturn(*lacing);
            ogg_segment(data, &current_packet_size, *lacing);
        }
    scrFinish(0);
}

size_t
write_callback(const void *data, __attribute__((unused)) size_t _, size_t nmemb,
               __attribute__((unused)) void *extra) {
    static uint8_t buffer[UINT8_MAX];
    static uint8_t buf_size = 0;
    static int requested = 0;
    const void *data_end = data + nmemb;
    for (size_t available = nmemb;
            available && requested >= 0; available = data_end - data) {
        uint8_t remaining = requested - buf_size;
        if (available < remaining || buf_size) {
            uint8_t chunk = available < remaining ? available : remaining;
            memcpy(buffer + buf_size, data, chunk);
            data += chunk;
            buf_size += chunk;
            if (buf_size == requested) {
                requested = scr__ogg_page(buffer);
                buf_size = 0;
            }
        } else {
            int new_request = scr__ogg_page(data);
            data += requested;
            requested = new_request;
        }
    }
    return requested >= 0 ? nmemb : (nmemb ? 0 : 1);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    char *test_argv[] = {argv[0], "https://radio.advance-rp.ru/channel1.ogg",
                         "Advance Radio"};
    if (argc < sizeof(test_argv) / sizeof(test_argv[0])) {
        argv = test_argv;
    }
    radio_name = argv[2];
    curl_global_init(0);
    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, argv[1]);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        fprintf(stderr, "%s: %s\n", argv[1], curl_easy_strerror(res));
    }
    ogg_track_end(false);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}
