/*
 * Copyright (C) 2013 The Android Open Source Project
 * Inspired by TinyHW, written by Mark Brown at Wolfson Micro
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "audio_route"
/*#define LOG_NDEBUG 0*/

#include <errno.h>
#include <expat.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <log/log.h>

#include <tinyalsa/asoundlib.h>

#define BUF_SIZE 1024
#define MIXER_XML_PATH "/system/etc/mixer_paths.xml"
#define INITIAL_MIXER_PATH_SIZE 8

enum update_direction {
    DIRECTION_FORWARD,
    DIRECTION_REVERSE,
    DIRECTION_REVERSE_RESET
};

/*
 "ctl_values" couples the buffer pointer with a variable "byte_size" to store
 both types of ctl setting as below in respective ways.

                    | fixed-length            | tlv-typed byte ctl [note 1]
                    | byte/int/enum ctl       |
 -------------------+-------------------------+--------------------------------
  alloc buffer size | num_values * size(type) | num_values * 1     [note 2]
                    +                         +
  stored value size | always as the full size | can be any size from 1 up to
                    | of allocated buffer     | num_values
                    +                         +
  "byte_size" value | equal to buffer size,   | equal to stored value size,
                    | fixed in runtime        | variable according to setting

 additional notes:
 [1] tlv-typed read/write is not a byte-specific feature but by now it only
     supports for byte ctls via Tinyalsa API
 [2] num_values is obtained from mixer_ctl_get_num_values()
 */
struct ctl_values {
    /* anonymous union */
    union {
        int *enumerated;
        long *integer;
        void *ptr;
        unsigned char *bytes;
    };
    unsigned int byte_size;
};

struct mixer_state {
    struct mixer_ctl *ctl;
    unsigned int num_values;
    struct ctl_values old_value;
    struct ctl_values new_value;
    struct ctl_values reset_value;
    unsigned int active_count;
};

struct mixer_setting {
    unsigned int ctl_index;
    unsigned int num_values;
    unsigned int type;
    struct ctl_values value;
};

struct mixer_value {
    unsigned int ctl_index;
    int index;
    long value;
    /*
     memory pointed by this is allocated in start_tag during parsing ctl of
     MIXER_CTL_TYPE_BYTE or MIXER_CTL_TYPE_INT, and is released after the
     parsed values are updated to either setting value within a path,
     or top level initial setting value
     */
    long *values;
    unsigned int num_values_in_array;
};

struct mixer_path {
    char *name;
    unsigned int size;
    unsigned int length;
    struct mixer_setting *setting;
};

struct audio_route {
    struct mixer *mixer;
    unsigned int num_mixer_ctls;
    struct mixer_state *mixer_state;

    unsigned int mixer_path_size;
    unsigned int num_mixer_paths;
    struct mixer_path *mixer_path;
};

struct config_parse_state {
    struct audio_route *ar;
    struct mixer_path *path;
    int level;
    bool enum_mixer_numeric_fallback;
};

static size_t sizeof_ctl_type(enum mixer_ctl_type type);

static bool ctl_is_tlv_byte_type(struct mixer_ctl *ctl)
{
    return mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_BYTE && mixer_ctl_is_access_tlv_rw(ctl);
}

/* ctl_values helper functions */

static int ctl_values_alloc(struct ctl_values *value, unsigned int num_values,
                            enum mixer_ctl_type type)
{
    void *ptr;
    size_t value_sz = sizeof_ctl_type(type);

    ptr = calloc(num_values, value_sz);
    if (!ptr)
        return -1;

    value->ptr = ptr;
    value->byte_size = num_values * value_sz;
    return 0;
}

static void ctl_values_copy(struct ctl_values *dst, const struct ctl_values *src)
{
    /*
     this should only be used for copying among "ctl_values"-es of a "mixer_state", all of them
     will be allocated the same size of buffers according to "num_values" obtained from mixer ctl.
     */
    memcpy(dst->ptr, src->ptr, src->byte_size);
    dst->byte_size = src->byte_size;
}

/* path functions */

static bool is_supported_ctl_type(enum mixer_ctl_type type)
{
    switch (type) {
    case MIXER_CTL_TYPE_BOOL:
    case MIXER_CTL_TYPE_INT:
    case MIXER_CTL_TYPE_ENUM:
    case MIXER_CTL_TYPE_BYTE:
        return true;
    default:
        return false;
    }
}

/* as they match in alsa */
static size_t sizeof_ctl_type(enum mixer_ctl_type type) {
    switch (type) {
    case MIXER_CTL_TYPE_BOOL:
    case MIXER_CTL_TYPE_INT:
        return sizeof(long);
    case MIXER_CTL_TYPE_ENUM:
        return sizeof(int);
    case MIXER_CTL_TYPE_BYTE:
        return sizeof(unsigned char);
    case MIXER_CTL_TYPE_INT64:
    case MIXER_CTL_TYPE_IEC958:
    case MIXER_CTL_TYPE_UNKNOWN:
    default:
        LOG_ALWAYS_FATAL("Unsupported mixer ctl type: %d, check type before calling", (int)type);
        return 0;
    }
}

static inline struct mixer_ctl *index_to_ctl(struct audio_route *ar,
                                             unsigned int ctl_index)
{
    return ar->mixer_state[ctl_index].ctl;
}

#if 0
static void path_print(struct audio_route *ar, struct mixer_path *path)
{
    unsigned int i;
    unsigned int j;

    ALOGE("Path: %s, length: %d", path->name, path->length);
    for (i = 0; i < path->length; i++) {
        struct mixer_ctl *ctl = index_to_ctl(ar, path->setting[i].ctl_index);

        ALOGE("  id=%d: ctl=%s", i, mixer_ctl_get_name(ctl));
        if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_BYTE) {
            for (j = 0; j < path->setting[i].num_values; j++)
                ALOGE("    id=%d value=0x%02x", j, path->setting[i].value.bytes[j]);
        } else if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_ENUM) {
            for (j = 0; j < path->setting[i].num_values; j++)
                ALOGE("    id=%d value=%d", j, path->setting[i].value.enumerated[j]);
        } else {
            for (j = 0; j < path->setting[i].num_values; j++)
                ALOGE("    id=%d value=%ld", j, path->setting[i].value.integer[j]);
        }
    }
}
#endif

static void path_free(struct audio_route *ar)
{
    unsigned int i;

    for (i = 0; i < ar->num_mixer_paths; i++) {
        free(ar->mixer_path[i].name);
        if (ar->mixer_path[i].setting) {
            size_t j;
            for (j = 0; j < ar->mixer_path[i].length; j++) {
                free(ar->mixer_path[i].setting[j].value.ptr);
            }
            free(ar->mixer_path[i].setting);
            ar->mixer_path[i].size = 0;
            ar->mixer_path[i].length = 0;
            ar->mixer_path[i].setting = NULL;
        }
    }
    free(ar->mixer_path);
    ar->mixer_path = NULL;
    ar->mixer_path_size = 0;
    ar->num_mixer_paths = 0;
}

static struct mixer_path *path_get_by_name(struct audio_route *ar,
                                           const char *name)
{
    unsigned int i;

    for (i = 0; i < ar->num_mixer_paths; i++)
        if (strcmp(ar->mixer_path[i].name, name) == 0)
            return &ar->mixer_path[i];

    return NULL;
}

static struct mixer_path *path_create(struct audio_route *ar, const char *name)
{
    struct mixer_path *new_mixer_path = NULL;

    if (path_get_by_name(ar, name)) {
        ALOGW("Path name '%s' already exists", name);
        return NULL;
    }

    /* check if we need to allocate more space for mixer paths */
    if (ar->mixer_path_size <= ar->num_mixer_paths) {
        if (ar->mixer_path_size == 0)
            ar->mixer_path_size = INITIAL_MIXER_PATH_SIZE;
        else
            ar->mixer_path_size *= 2;

        new_mixer_path = realloc(ar->mixer_path, ar->mixer_path_size *
                                 sizeof(struct mixer_path));
        if (new_mixer_path == NULL) {
            ALOGE("Unable to allocate more paths");
            return NULL;
        } else {
            ar->mixer_path = new_mixer_path;
        }
    }

    /* initialise the new mixer path */
    ar->mixer_path[ar->num_mixer_paths].name = strdup(name);
    ar->mixer_path[ar->num_mixer_paths].size = 0;
    ar->mixer_path[ar->num_mixer_paths].length = 0;
    ar->mixer_path[ar->num_mixer_paths].setting = NULL;

    /* return the mixer path just added, then increment number of them */
    return &ar->mixer_path[ar->num_mixer_paths++];
}

static int find_ctl_index_in_path(struct mixer_path *path,
                                  unsigned int ctl_index)
{
    unsigned int i;

    for (i = 0; i < path->length; i++)
        if (path->setting[i].ctl_index == ctl_index)
            return i;

    return -1;
}

static int alloc_path_setting(struct mixer_path *path)
{
    struct mixer_setting *new_path_setting;
    int path_index;

    /* check if we need to allocate more space for path settings */
    if (path->size <= path->length) {
        if (path->size == 0)
            path->size = INITIAL_MIXER_PATH_SIZE;
        else
            path->size *= 2;

        new_path_setting = realloc(path->setting,
                                   path->size * sizeof(struct mixer_setting));
        if (new_path_setting == NULL) {
            ALOGE("Unable to allocate more path settings");
            return -1;
        } else {
            path->setting = new_path_setting;
        }
    }

    path_index = path->length;
    path->length++;

    return path_index;
}

static int path_add_setting(struct audio_route *ar, struct mixer_path *path,
                            struct mixer_setting *setting)
{
    int path_index;
    int rc;

    if (find_ctl_index_in_path(path, setting->ctl_index) != -1) {
        struct mixer_ctl *ctl = index_to_ctl(ar, setting->ctl_index);

        ALOGW("Control '%s' already exists in path '%s' - Ignore one in the new sub path",
              mixer_ctl_get_name(ctl), path->name);
        return -2;
    }

    if (!is_supported_ctl_type(setting->type)) {
        ALOGE("unsupported type %d", (int)setting->type);
        return -1;
    }

    path_index = alloc_path_setting(path);
    if (path_index < 0)
        return -1;

    path->setting[path_index].ctl_index = setting->ctl_index;
    path->setting[path_index].type = setting->type;
    path->setting[path_index].num_values = setting->num_values;

    rc = ctl_values_alloc(&path->setting[path_index].value, setting->num_values, setting->type);
    if (rc < 0) {
        ALOGE("failed to allocate mem for path setting");
        return rc;
    }
    /* copy all values */
    ctl_values_copy(&path->setting[path_index].value, &setting->value);

    return 0;
}

static int path_add_value(struct audio_route *ar, struct mixer_path *path,
                          struct mixer_value *mixer_value)
{
    unsigned int i;
    int path_index;
    unsigned int num_values;
    struct mixer_ctl *ctl;
    int rc;

    /* Check that mixer value index is within range */
    ctl = index_to_ctl(ar, mixer_value->ctl_index);
    num_values = mixer_ctl_get_num_values(ctl);
    if (mixer_value->index >= (int)num_values) {
        ALOGE("mixer index %d is out of range for '%s'", mixer_value->index,
              mixer_ctl_get_name(ctl));
        return -1;
    }

    path_index = find_ctl_index_in_path(path, mixer_value->ctl_index);
    if (path_index < 0) {
        /* New path */

        enum mixer_ctl_type type = mixer_ctl_get_type(ctl);
        if (!is_supported_ctl_type(type)) {
            ALOGE("unsupported type %d", (int)type);
            return -1;
        }
        path_index = alloc_path_setting(path);
        if (path_index < 0)
            return -1;

        /* initialise the new path setting */
        path->setting[path_index].ctl_index = mixer_value->ctl_index;
        path->setting[path_index].num_values = num_values;
        path->setting[path_index].type = type;

        rc = ctl_values_alloc(&path->setting[path_index].value, num_values, type);
        if (rc < 0) {
            ALOGE("failed to allocate mem for path setting");
            return rc;
        }
        if (path->setting[path_index].type == MIXER_CTL_TYPE_BYTE)
            path->setting[path_index].value.bytes[0] = mixer_value->value;
        else if (path->setting[path_index].type == MIXER_CTL_TYPE_ENUM)
            path->setting[path_index].value.enumerated[0] = mixer_value->value;
        else
            path->setting[path_index].value.integer[0] = mixer_value->value;
    }

    if (mixer_value->index == -1) {
        /* set all values the same except for CTL_TYPE_BYTE and CTL_TYPE_INT */
        if (path->setting[path_index].type == MIXER_CTL_TYPE_BYTE) {
            /* update the number of values (bytes) from input "mixer_value" */
            for (i = 0; i < mixer_value->num_values_in_array; i++)
                path->setting[path_index].value.bytes[i] = mixer_value->values[i];
            path->setting[path_index].value.byte_size = mixer_value->num_values_in_array;
        } else if (path->setting[path_index].type == MIXER_CTL_TYPE_INT) {
            for (i = 0; i < num_values; i++)
                path->setting[path_index].value.integer[i] = mixer_value->values[i];
        } else if (path->setting[path_index].type == MIXER_CTL_TYPE_ENUM) {
            for (i = 0; i < num_values; i++)
                path->setting[path_index].value.enumerated[i] = mixer_value->value;
        } else {
            for (i = 0; i < num_values; i++)
                path->setting[path_index].value.integer[i] = mixer_value->value;
        }
    } else {
        /* set only one value */
        if (path->setting[path_index].type == MIXER_CTL_TYPE_BYTE)
            path->setting[path_index].value.bytes[mixer_value->index] = mixer_value->value;
        else if (path->setting[path_index].type == MIXER_CTL_TYPE_ENUM)
            path->setting[path_index].value.enumerated[mixer_value->index] = mixer_value->value;
        else
            path->setting[path_index].value.integer[mixer_value->index] = mixer_value->value;
    }

    return 0;
}

static int path_add_path(struct audio_route *ar, struct mixer_path *path,
                         struct mixer_path *sub_path)
{
    unsigned int i;

    for (i = 0; i < sub_path->length; i++) {
        int retVal = path_add_setting(ar, path, &sub_path->setting[i]);
        if (retVal < 0) {
            if (retVal == -2)
                continue;
            else
                return -1;
        }
    }
    return 0;
}

static int path_apply(struct audio_route *ar, struct mixer_path *path)
{
    unsigned int i;
    unsigned int ctl_index;
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;

    ALOGD("Apply path: %s", path->name != NULL ? path->name : "none");
    for (i = 0; i < path->length; i++) {
        ctl_index = path->setting[i].ctl_index;
        ctl = index_to_ctl(ar, ctl_index);
        type = mixer_ctl_get_type(ctl);
        if (!is_supported_ctl_type(type))
            continue;
        ctl_values_copy(&ar->mixer_state[ctl_index].new_value, &path->setting[i].value);
    }

    return 0;
}

static int path_reset(struct audio_route *ar, struct mixer_path *path)
{
    unsigned int i;
    unsigned int ctl_index;
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;

    ALOGV("Reset path: %s", path->name != NULL ? path->name : "none");
    for (i = 0; i < path->length; i++) {
        ctl_index = path->setting[i].ctl_index;
        ctl = index_to_ctl(ar, ctl_index);
        type = mixer_ctl_get_type(ctl);
        if (!is_supported_ctl_type(type))
            continue;
        /* reset the value(s) */
        ctl_values_copy(&ar->mixer_state[ctl_index].new_value,
                        &ar->mixer_state[ctl_index].reset_value);
    }

    return 0;
}

static bool safe_strtol(const char *str, long *val)
{
    char *end;
    long v;
    if (str == NULL || strlen(str) == 0)
        return false;
    errno = 0;
    v = strtol(str, &end, 0);
    if (errno || *end)
        return false;
    *val = v;
    return true;
}

/* mixer helper function */
static int mixer_enum_string_to_value(struct mixer_ctl *ctl, const char *string,
                                      bool allow_numeric_fallback)
{
    unsigned int i;
    unsigned int num_values = mixer_ctl_get_num_enums(ctl);

    if (string == NULL) {
        ALOGE("NULL enum value string passed to mixer_enum_string_to_value() for ctl %s",
              mixer_ctl_get_name(ctl));
        return 0;
    }

    /* Search the enum strings for a particular one */
    for (i = 0; i < num_values; i++) {
        if (strcmp(mixer_ctl_get_enum_string(ctl, i), string) == 0)
            break;
    }
    if (i == num_values) {
        /* No enum string match. Check the flag before numeric parsing. */
        if (allow_numeric_fallback) {
            long value = 0;
            if (safe_strtol(string, &value) && value >= 0 && value < num_values) {
                return value;
            }
        }
        ALOGW("unknown enum value string %s for ctl %s",
              string, mixer_ctl_get_name(ctl));
        return 0;
    }
    return i;
}

static int mixer_get_bytes_from_file(long **value_array, const char *filepath,
                                     unsigned int max_bytes)
{
    unsigned char *buf = NULL;
    long *values = NULL;
    int bytes_read = -1;
    unsigned int i;

    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ALOGE("Failed to open %s: %s", filepath, strerror(errno));
        return -1;
    }

    buf = calloc(max_bytes, 1);
    if (!buf) {
        ALOGE("failed to allocate mem for file read buffer");
        goto exit;
    }

    bytes_read = fread(buf, 1, max_bytes, file);
    if (bytes_read < 0) {
        ALOGE("failed to read data from file, rc: %d", bytes_read);
        goto exit;
    }

    values = calloc(bytes_read, sizeof(long));
    if (!values) {
        ALOGE("failed to allocate mem for values array");
        bytes_read = -1;
        goto exit;
    }

    for (i = 0; i < bytes_read; i++) {
        values[i] = (long)buf[i];
    }
    *value_array = values;

exit:
    free(buf);
    fclose(file);
    return bytes_read;
}

static void start_tag(void *data, const XML_Char *tag_name,
                      const XML_Char **attr)
{
    const XML_Char *attr_name = NULL;
    const XML_Char *attr_id = NULL;
    const XML_Char *attr_value = NULL;
    const XML_Char *attr_enum_mixer_numeric_fallback = NULL;
    const XML_Char *attr_bin = NULL;
    struct config_parse_state *state = data;
    struct audio_route *ar = state->ar;
    unsigned int i;
    unsigned int ctl_index;
    struct mixer_ctl *ctl;
    long value;
    unsigned int id;
    struct mixer_value mixer_value;
    enum mixer_ctl_type type;
    long* value_array = NULL;
    unsigned int num_values_in_array = 0;

    /* Get name, id and value attributes (these may be empty) */
    for (i = 0; attr[i]; i += 2) {
        if (strcmp(attr[i], "name") == 0)
            attr_name = attr[i + 1];
        else if (strcmp(attr[i], "id") == 0)
            attr_id = attr[i + 1];
        else if (strcmp(attr[i], "value") == 0)
            attr_value = attr[i + 1];
        else if (strcmp(attr[i], "enum_mixer_numeric_fallback") == 0)
            attr_enum_mixer_numeric_fallback = attr[i + 1];
        else if (strcmp(attr[i], "bin") == 0)
            attr_bin = attr[i + 1];
    }

    /* Look at tags */
    if (strcmp(tag_name, "mixer") == 0) {
        state->enum_mixer_numeric_fallback =
                attr_enum_mixer_numeric_fallback != NULL &&
                strcmp(attr_enum_mixer_numeric_fallback, "true") == 0 ;
    } else if (strcmp(tag_name, "path") == 0) {
        if (attr_name == NULL) {
            ALOGE("Unnamed path!");
        } else {
            if (state->level == 1) {
                /* top level path: create and stash the path */
                state->path = path_create(ar, (char *)attr_name);
                if (state->path == NULL)
                    ALOGW("path creation failed, please check if the path exists");
            } else {
                /* nested path */
                struct mixer_path *sub_path = path_get_by_name(ar, attr_name);
                if (!sub_path) {
                    ALOGW("unable to find sub path '%s'", attr_name);
                } else if (state->path != NULL) {
                    path_add_path(ar, state->path, sub_path);
                }
            }
        }
    } else if (strcmp(tag_name, "ctl") == 0) {
        /* Obtain the mixer ctl and value */
        ctl = mixer_get_ctl_by_name(ar->mixer, attr_name);
        if (ctl == NULL) {
            ALOGW("Control '%s' doesn't exist - skipping", attr_name);
            goto done;
        }

        switch (mixer_ctl_get_type(ctl)) {
        case MIXER_CTL_TYPE_BOOL:
            if (attr_value == NULL) {
                ALOGE("No value specified for ctl %s", attr_name);
                goto done;
            }
            value = strtol((char *)attr_value, NULL, 0);
            break;
        case MIXER_CTL_TYPE_INT:
        case MIXER_CTL_TYPE_BYTE: {
                char *attr_sub_value, *test_r;
                unsigned int num_values = mixer_ctl_get_num_values(ctl);

                if (attr_bin && mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_BYTE) {
                    /* get byte values from binfile */
                    int bytes_read = mixer_get_bytes_from_file(&value_array, attr_bin, num_values);
                    if (bytes_read <= 0) {
                        ALOGE("failed to get bytes from file '%s'", attr_bin);
                        goto done;
                    }
                    if (bytes_read < num_values && mixer_ctl_is_access_tlv_rw(ctl) == 0) {
                        ALOGE("expect %d values but only %d specified for ctl %s",
                              num_values, bytes_read, attr_name);
                        goto done;
                    }
                    num_values_in_array = bytes_read;
                    break;
                }

                if (attr_value == NULL) {
                    ALOGE("No value specified for ctl %s", attr_name);
                    goto done;
                }
                value_array = calloc(num_values, sizeof(long));
                if (!value_array) {
                    ALOGE("failed to allocate mem for ctl %s", attr_name);
                    goto done;
                }
                for (i = 0; i < num_values; i++) {
                    attr_sub_value = strtok_r((char *)attr_value, " ", &test_r);
                    if (attr_sub_value == NULL) {
                        /* the length of setting for tlv-typed byte control
                           can be any size up to num_value; break the loop so
                           the current count of values will be recorded */
                        if (ctl_is_tlv_byte_type(ctl))
                            break;

                        ALOGE("expect %d values but only %d specified for ctl %s",
                            num_values, i, attr_name);
                        goto done;
                    }
                    if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_INT)
                        value_array[i] = strtol((char *)attr_sub_value, NULL, 0);
                    else
                        value_array[i] =
                           (unsigned char) strtol((char *)attr_sub_value, NULL, 16);

                    /* count the number of values written in array */
                    num_values_in_array++;

                    if (attr_id)
                        break;

                    attr_value = NULL;
                }
            } break;
        case MIXER_CTL_TYPE_ENUM:
            if (attr_value == NULL) {
                ALOGE("No value specified for ctl %s", attr_name);
                goto done;
            }
            value = mixer_enum_string_to_value(ctl, (char *)attr_value,
                                               state->enum_mixer_numeric_fallback);
            break;
        default:
            value = 0;
            break;
        }

        /* locate the mixer ctl in the list */
        for (ctl_index = 0; ctl_index < ar->num_mixer_ctls; ctl_index++) {
            if (ar->mixer_state[ctl_index].ctl == ctl)
                break;
        }

        if (state->level == 1) {
            /* top level ctl (initial setting) */

            type = mixer_ctl_get_type(ctl);
            if (is_supported_ctl_type(type)) {
                /* apply the new value */
                if (attr_id) {
                    /* set only one value */
                    id = atoi((char *)attr_id);
                    if (id < ar->mixer_state[ctl_index].num_values)
                        if (type == MIXER_CTL_TYPE_BYTE)
                            ar->mixer_state[ctl_index].new_value.bytes[id] = value_array[0];
                        else if (type == MIXER_CTL_TYPE_INT)
                            ar->mixer_state[ctl_index].new_value.integer[id] = value_array[0];
                        else if (type == MIXER_CTL_TYPE_ENUM)
                            ar->mixer_state[ctl_index].new_value.enumerated[id] = value;
                        else
                            ar->mixer_state[ctl_index].new_value.integer[id] = value;
                    else
                        ALOGW("value id out of range for mixer ctl '%s'",
                              mixer_ctl_get_name(ctl));
                } else if (ctl_is_tlv_byte_type(ctl)) {
                    /* for tlv-typed ctl, only set the number of values (bytes) carried by array,
                       and update the number of bytes */
                    for (i = 0; i < num_values_in_array; i++)
                        ar->mixer_state[ctl_index].new_value.bytes[i] = value_array[i];
                    ar->mixer_state[ctl_index].new_value.byte_size = num_values_in_array;
                } else {
                    /* set all values the same except for CTL_TYPE_BYTE and CTL_TYPE_INT */
                    for (i = 0; i < ar->mixer_state[ctl_index].num_values; i++)
                        if (type == MIXER_CTL_TYPE_BYTE)
                            ar->mixer_state[ctl_index].new_value.bytes[i] = value_array[i];
                        else if (type == MIXER_CTL_TYPE_INT)
                            ar->mixer_state[ctl_index].new_value.integer[i] = value_array[i];
                        else if (type == MIXER_CTL_TYPE_ENUM)
                            ar->mixer_state[ctl_index].new_value.enumerated[i] = value;
                        else
                            ar->mixer_state[ctl_index].new_value.integer[i] = value;
                }
            }
        } else {
            /* nested ctl (within a path) */
            mixer_value.ctl_index = ctl_index;
            if (mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_BYTE ||
                mixer_ctl_get_type(ctl) == MIXER_CTL_TYPE_INT) {
                mixer_value.values = value_array;
                mixer_value.value = value_array[0];
                mixer_value.num_values_in_array = num_values_in_array;
            } else {
                mixer_value.value = value;
            }
            if (attr_id)
                mixer_value.index = atoi((char *)attr_id);
            else
                mixer_value.index = -1;
            if (state->path != NULL)
                path_add_value(ar, state->path, &mixer_value);
        }
    }

done:
    free(value_array);
    state->level++;
}

static void end_tag(void *data, const XML_Char *tag_name)
{
    struct config_parse_state *state = data;
    (void)tag_name;

    state->level--;
}

static int alloc_mixer_state(struct audio_route *ar)
{
    unsigned int i;
    unsigned int num_values;
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;

    ar->num_mixer_ctls = mixer_get_num_ctls(ar->mixer);
    ar->mixer_state = calloc(ar->num_mixer_ctls, sizeof(struct mixer_state));
    if (!ar->mixer_state)
        return -1;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        ctl = mixer_get_ctl(ar->mixer, i);
        num_values = mixer_ctl_get_num_values(ctl);

        ar->mixer_state[i].ctl = ctl;
        ar->mixer_state[i].num_values = num_values;
        ar->mixer_state[i].active_count = 0;

        /* Skip unsupported types that are not supported yet in XML */
        type = mixer_ctl_get_type(ctl);

        if (!is_supported_ctl_type(type))
            continue;

        /*
         for tlv-typed ctl, "mixer_ctl_get_num_values()" returns the max length of a
         setting data. The buffer size allocated per mixer setting should align the
         max length to be capable of carrying any length of data.
         */
        ctl_values_alloc(&ar->mixer_state[i].old_value, num_values, type);
        ctl_values_alloc(&ar->mixer_state[i].new_value, num_values, type);
        ctl_values_alloc(&ar->mixer_state[i].reset_value, num_values, type);

        if (type == MIXER_CTL_TYPE_ENUM)
            ar->mixer_state[i].old_value.enumerated[0] = mixer_ctl_get_value(ctl, 0);
        else
            mixer_ctl_get_array(ctl, ar->mixer_state[i].old_value.ptr, num_values);

        ctl_values_copy(&ar->mixer_state[i].new_value, &ar->mixer_state[i].old_value);
    }

    return 0;
}

static void free_mixer_state(struct audio_route *ar)
{
    unsigned int i;
    enum mixer_ctl_type type;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        type = mixer_ctl_get_type(ar->mixer_state[i].ctl);
        if (!is_supported_ctl_type(type))
            continue;

        free(ar->mixer_state[i].old_value.ptr);
        free(ar->mixer_state[i].new_value.ptr);
        free(ar->mixer_state[i].reset_value.ptr);
    }

    free(ar->mixer_state);
    ar->mixer_state = NULL;
}

static void mixer_set_value_if_changed(struct mixer_state *ms)
{
    unsigned int i;
    struct mixer_ctl *ctl = ms->ctl;
    enum mixer_ctl_type type = mixer_ctl_get_type(ctl);

    if (type == MIXER_CTL_TYPE_BYTE) {
        unsigned int num_bytes;
        /*
         for tlv-typed ctl, "mixer_ctl_set_array()" should specify the length of data to
         be set, thus the data can be wrapped into tlv format correctly by Tinyalsa.
         */
        num_bytes = ctl_is_tlv_byte_type(ctl) ? ms->new_value.byte_size : ms->num_values;
        for (i = 0; i < num_bytes; i++) {
            if (ms->old_value.bytes[i] != ms->new_value.bytes[i]) {
                mixer_ctl_set_array(ctl, ms->new_value.ptr, num_bytes);
                ctl_values_copy(&ms->old_value, &ms->new_value);
                return;
            }
        }
    } else if (type == MIXER_CTL_TYPE_ENUM) {
        for (i = 0; i < ms->num_values; i++) {
            if (ms->old_value.enumerated[i] != ms->new_value.enumerated[i]) {
                mixer_ctl_set_value(ctl, 0, ms->new_value.enumerated[0]);
                ctl_values_copy(&ms->old_value, &ms->new_value);
                return;
            }
        }
    } else {
        for (i = 0; i < ms->num_values; i++) {
            if (ms->old_value.integer[i] != ms->new_value.integer[i]) {
                mixer_ctl_set_array(ctl, ms->new_value.ptr, ms->num_values);
                ctl_values_copy(&ms->old_value, &ms->new_value);
                return;
            }
        }
    }
}

/* Update the mixer with any changed values */
int audio_route_update_mixer(struct audio_route *ar)
{
    unsigned int i;
    struct mixer_ctl *ctl;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        enum mixer_ctl_type type;

        ctl = ar->mixer_state[i].ctl;

        /* Skip unsupported types */
        type = mixer_ctl_get_type(ctl);
        if (!is_supported_ctl_type(type))
            continue;

        /* if the value has changed, update the mixer */
        mixer_set_value_if_changed(&ar->mixer_state[i]);
    }

    return 0;
}

/* saves the current state of the mixer, for resetting all controls */
static void save_mixer_state(struct audio_route *ar)
{
    unsigned int i;
    enum mixer_ctl_type type;

    for (i = 0; i < ar->num_mixer_ctls; i++) {
        type = mixer_ctl_get_type(ar->mixer_state[i].ctl);
        if (!is_supported_ctl_type(type))
            continue;

        ctl_values_copy(&ar->mixer_state[i].reset_value, &ar->mixer_state[i].new_value);
    }
}

/* Reset the audio routes back to the initial state */
void audio_route_reset(struct audio_route *ar)
{
    unsigned int i;
    enum mixer_ctl_type type;

    /* load all of the saved values */
    for (i = 0; i < ar->num_mixer_ctls; i++) {
        type = mixer_ctl_get_type(ar->mixer_state[i].ctl);
        if (!is_supported_ctl_type(type))
            continue;

        ctl_values_copy(&ar->mixer_state[i].new_value, &ar->mixer_state[i].reset_value);
    }
}

/* Apply an audio route path by name */
int audio_route_apply_path(struct audio_route *ar, const char *name)
{
    struct mixer_path *path;

    if (!ar) {
        ALOGE("invalid audio_route");
        return -1;
    }

    path = path_get_by_name(ar, name);
    if (!path) {
        ALOGE("unable to find path '%s'", name);
        return -1;
    }

    path_apply(ar, path);

    return 0;
}

/* Reset an audio route path by name */
int audio_route_reset_path(struct audio_route *ar, const char *name)
{
    struct mixer_path *path;

    if (!ar) {
        ALOGE("invalid audio_route");
        return -1;
    }

    path = path_get_by_name(ar, name);
    if (!path) {
        ALOGE("unable to find path '%s'", name);
        return -1;
    }

    path_reset(ar, path);

    return 0;
}

/*
 * Operates on the specified path .. controls will be updated in the
 * order listed in the XML file
 */
static int audio_route_update_path(struct audio_route *ar, const char *name, int direction)
{
    struct mixer_path *path;
    unsigned int j;
    bool reverse = direction != DIRECTION_FORWARD;
    bool force_reset = direction == DIRECTION_REVERSE_RESET;

    if (!ar) {
        ALOGE("invalid audio_route");
        return -1;
    }

    path = path_get_by_name(ar, name);
    if (!path) {
        ALOGE("unable to find path '%s'", name);
        return -1;
    }

    for (size_t i = 0; i < path->length; ++i) {
        unsigned int ctl_index;
        enum mixer_ctl_type type;

        ctl_index = path->setting[reverse ? path->length - 1 - i : i].ctl_index;

        struct mixer_state * ms = &ar->mixer_state[ctl_index];

        type = mixer_ctl_get_type(ms->ctl);
        if (!is_supported_ctl_type(type)) {
            continue;
        }

        if (reverse && ms->active_count > 0) {
            if (force_reset) {
                ms->active_count = 0;
            } else if (--ms->active_count > 0) {
                ALOGD("%s: skip to reset mixer control '%s' in path '%s' "
                    "because it is still needed by other paths", __func__,
                    mixer_ctl_get_name(ms->ctl), name);
                ctl_values_copy(&ms->new_value, &ms->old_value);
                continue;
            }
        } else if (!reverse) {
            ms->active_count++;
        }

        /* if any value has changed, update the mixer */
        mixer_set_value_if_changed(ms);
    }
    return 0;
}

int audio_route_apply_and_update_path(struct audio_route *ar, const char *name)
{
    if (audio_route_apply_path(ar, name) < 0) {
        return -1;
    }
    return audio_route_update_path(ar, name, DIRECTION_FORWARD);
}

int audio_route_reset_and_update_path(struct audio_route *ar, const char *name)
{
    if (audio_route_reset_path(ar, name) < 0) {
        return -1;
    }
    return audio_route_update_path(ar, name, DIRECTION_REVERSE);
}

int audio_route_force_reset_and_update_path(struct audio_route *ar, const char *name)
{
    if (audio_route_reset_path(ar, name) < 0) {
        return -1;
    }

    return audio_route_update_path(ar, name, DIRECTION_REVERSE_RESET);
}

int audio_route_supports_path(struct audio_route *ar, const char *name)
{
    if (!path_get_by_name(ar, name)) {
        return -1;
    }

    return 0;
}

struct audio_route *audio_route_init(unsigned int card, const char *xml_path)
{
    struct config_parse_state state;
    XML_Parser parser;
    FILE *file;
    int bytes_read;
    void *buf;
    struct audio_route *ar;

    ar = calloc(1, sizeof(struct audio_route));
    if (!ar)
        goto err_calloc;

    ar->mixer = mixer_open(card);
    if (!ar->mixer) {
        ALOGE("Unable to open the mixer, aborting.");
        goto err_mixer_open;
    }

    ar->mixer_path = NULL;
    ar->mixer_path_size = 0;
    ar->num_mixer_paths = 0;

    /* allocate space for and read current mixer settings */
    if (alloc_mixer_state(ar) < 0)
        goto err_mixer_state;

    /* use the default XML path if none is provided */
    if (xml_path == NULL)
        xml_path = MIXER_XML_PATH;

    file = fopen(xml_path, "r");

    if (!file) {
        ALOGE("Failed to open %s: %s", xml_path, strerror(errno));
        goto err_fopen;
    }

    parser = XML_ParserCreate(NULL);
    if (!parser) {
        ALOGE("Failed to create XML parser");
        goto err_parser_create;
    }

    memset(&state, 0, sizeof(state));
    state.ar = ar;
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, start_tag, end_tag);

    for (;;) {
        buf = XML_GetBuffer(parser, BUF_SIZE);
        if (buf == NULL)
            goto err_parse;

        bytes_read = fread(buf, 1, BUF_SIZE, file);
        if (bytes_read < 0)
            goto err_parse;

        if (XML_ParseBuffer(parser, bytes_read,
                            bytes_read == 0) == XML_STATUS_ERROR) {
            ALOGE("Error in mixer xml (%s)", MIXER_XML_PATH);
            goto err_parse;
        }

        if (bytes_read == 0)
            break;
    }

    /* apply the initial mixer values, and save them so we can reset the
       mixer to the original values */
    audio_route_update_mixer(ar);
    save_mixer_state(ar);

    XML_ParserFree(parser);
    fclose(file);
    return ar;

err_parse:
    path_free(ar);
    XML_ParserFree(parser);
err_parser_create:
    fclose(file);
err_fopen:
    free_mixer_state(ar);
err_mixer_state:
    mixer_close(ar->mixer);
err_mixer_open:
    free(ar);
    ar = NULL;
err_calloc:
    return NULL;
}

void audio_route_free(struct audio_route *ar)
{
    free_mixer_state(ar);
    mixer_close(ar->mixer);
    path_free(ar);
    free(ar);
}
