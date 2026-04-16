// index.c — Staging area implementation
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755

// Forward declaration for object_write (from object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Comparator for sorting index entries by path
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    int idx = -1;
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return -1;
    for (int i = idx; i < index->count - 1; i++) {
        index->entries[i] = index->entries[i+1];
    }
    index->count--;
    return 0;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED FUNCTIONS ──────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) && index->count < MAX_INDEX_ENTRIES) {
        uint32_t mode, size;
        uint64_t mtime;
        char hash_hex[HASH_HEX_SIZE + 1];
        char path[512];
        if (sscanf(line, "%o %64s %lu %u %511s", &mode, hash_hex, &mtime, &size, path) == 5) {
            IndexEntry *e = &index->entries[index->count];
            e->mode = mode;
            hex_to_hash(hash_hex, &e->hash);
            e->mtime_sec = mtime;
            e->size = size;
            strcpy(e->path, path);
            index->count++;
        }
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    // Create a sorted copy of entries (required for deterministic output)
    IndexEntry *sorted = malloc(index->count * sizeof(IndexEntry));
    if (!sorted && index->count > 0) return -1;
    memcpy(sorted, index->entries, index->count * sizeof(IndexEntry));
    qsort(sorted, index->count, sizeof(IndexEntry), compare_index_entries);

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(temp_path, "w");
    if (!f) {
        free(sorted);
        return -1;
    }

    for (int i = 0; i < index->count; i++) {
        const IndexEntry *e = &sorted[i];
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %lu %u %s\n", e->mode, hex, e->mtime_sec, e->size, e->path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    if (rename(temp_path, INDEX_FILE) != 0) {
        unlink(temp_path);
        return -1;
    }
    return 0;
}

int index_add(Index *index, const char *path) {
    // Read file content
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(size);
    if (!data) { fclose(f); return -1; }
    size_t nread = fread(data, 1, size, f);
    fclose(f);
    if (nread != (size_t)size) { free(data); return -1; }

    // Write as blob
    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    // Get file metadata
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    uint32_t mode = MODE_FILE;
    if (st.st_mode & S_IXUSR) mode = MODE_EXEC;

    // Update or add entry
    IndexEntry *entry = index_find(index, path);
    if (entry) {
        entry->mode = mode;
        entry->hash = id;
        entry->mtime_sec = st.st_mtime;
        entry->size = size;
    } else if (index->count < MAX_INDEX_ENTRIES) {
        entry = &index->entries[index->count++];
        entry->mode = mode;
        entry->hash = id;
        entry->mtime_sec = st.st_mtime;
        entry->size = size;
        strcpy(entry->path, path);
    } else {
        return -1;
    }

    // Persist the index
    if (index_save(index) != 0) return -1;
    return 0;
}