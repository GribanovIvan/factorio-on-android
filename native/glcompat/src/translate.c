/* GLSL 3.30 desktop -> GLSL ES 3.20 translation.
 *
 * The game ships 110 shaders, all exactly "#version 330".  The translation is
 * deliberately conservative: keep the shader body byte-for-byte where
 * possible, add the precision qualifiers GLSL ES requires, map the small set
 * of desktop spellings the corpus uses, and give gl_InstanceID a base-instance
 * offset so glDrawArraysInstancedBaseInstance() can be emulated on ES where
 * base-instance draws do not exist.
 */
#include "glcompat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_precision =
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp sampler3D;\n"
    "precision highp samplerCube;\n"
    "precision highp sampler2DArray;\n"
    "precision highp samplerBuffer;\n"
    "precision highp sampler2DShadow;\n"
    "precision highp samplerCubeShadow;\n"
    "precision highp sampler2DArrayShadow;\n"
    "precision highp isampler2D;\n"
    "precision highp isampler3D;\n"
    "precision highp isamplerCube;\n"
    "precision highp isampler2DArray;\n"
    "precision highp isamplerBuffer;\n"
    "precision highp usampler2D;\n"
    "precision highp usampler3D;\n"
    "precision highp usamplerCube;\n"
    "precision highp usampler2DArray;\n"
    "precision highp usamplerBuffer;\n"
    "uniform int fsgl_BaseInstance;\n";

static bool is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Replace whole identifiers only.  This intentionally does not touch string
 * literals or comments; the corpus has neither around these spellings. */
static char *replace_ident(const char *src, const char *from, const char *to)
{
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    size_t cap = strlen(src) + 64;
    char *out = (char *)malloc(cap);
    size_t used = 0;
    if (!out) {
        return NULL;
    }
    const char *p = src;
    while (*p) {
        if (strncmp(p, from, from_len) == 0 &&
            (p == src || !is_ident(p[-1])) &&
            !is_ident(p[from_len])) {
            size_t need = used + to_len + 1;
            if (need > cap) {
                cap = need * 2;
                char *tmp = (char *)realloc(out, cap);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            memcpy(out + used, to, to_len);
            used += to_len;
            p += from_len;
        } else {
            if (used + 2 > cap) {
                cap *= 2;
                char *tmp = (char *)realloc(out, cap);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    return out;
}

static char *replace_all_ident(char *src, const char *from, const char *to)
{
    char *next = replace_ident(src, from, to);
    if (!next) {
        return src;
    }
    free(src);
    return next;
}

static bool contains_ident(const char *src, const char *needle)
{
    size_t n = strlen(needle);
    const char *p = src;
    while ((p = strstr(p, needle)) != NULL) {
        if ((p == src || !is_ident(p[-1])) && !is_ident(p[n])) {
            return true;
        }
        p += n;
    }
    return false;
}

char *fsgl_translate_shader(const char *source, GLenum stage, bool *changed)
{
    if (!source) {
        return NULL;
    }
    if (changed) {
        *changed = false;
    }

    const char *p = source;
    const char *version_start = NULL;
    const char *version_end = NULL;
    while (*p) {
        const char *line = p;
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *q = line;
        while (q < end && (*q == ' ' || *q == '\t')) {
            q++;
        }
        if (strncmp(q, "#version", 8) == 0) {
            version_start = line;
            version_end = end;
            break;
        }
        p = nl ? nl + 1 : end;
    }

    size_t prefix_len = version_start ? (size_t)(version_end - source) : 0;
    const char *body = version_start ? (version_end < source + strlen(source) ? version_end + 1 : version_end) : source;

    size_t cap = strlen(source) + strlen(k_precision) + 128;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t used = 0;

    if (version_start) {
        /* Preserve a version comment if one is present on the same line;
         * the corpus has none, but this keeps the translator honest. */
        const char *comment = memchr(version_start, '/', prefix_len);
        if (comment && comment + 1 < version_end && comment[1] == '/') {
            used += (size_t)snprintf(out + used, cap - used, "#version 320 es %.*s\n",
                                     (int)(version_end - comment), comment);
        } else {
            used += (size_t)snprintf(out + used, cap - used, "#version 320 es\n");
        }
        if (changed) {
            *changed = true;
        }
    }

    used += (size_t)snprintf(out + used, cap - used, "%s", k_precision);
    used += (size_t)snprintf(out + used, cap - used, "%s", body);

    /* Desktop spellings that appear in older assets. */
    out = replace_all_ident(out, "texture2DProjLod", "textureProjLod");
    out = replace_all_ident(out, "texture2DLod", "textureLod");
    out = replace_all_ident(out, "textureCubeLod", "textureLod");
    out = replace_all_ident(out, "texture2DGrad", "textureGrad");
    out = replace_all_ident(out, "textureCubeGrad", "textureGrad");
    out = replace_all_ident(out, "texture2DProj", "textureProj");
    out = replace_all_ident(out, "texture2D", "texture");
    out = replace_all_ident(out, "textureCube", "texture");

    if (contains_ident(out, "gl_InstanceID")) {
        out = replace_all_ident(out, "gl_InstanceID", "(gl_InstanceID + fsgl_BaseInstance)");
        if (changed) {
            *changed = true;
        }
    }

    if (stage == GL_FRAGMENT_SHADER) {
        if (contains_ident(out, "gl_FragColor") || contains_ident(out, "gl_FragData")) {
            /* Add a single output before the first function definition. */
            char *decl = strdup("layout(location = 0) out vec4 fsgl_FragColor;\n");
            char *pos = strstr(out, "void main");
            if (!decl) {
                free(out);
                return NULL;
            }
            if (pos) {
                size_t before = (size_t)(pos - out);
                char *merged = (char *)malloc(strlen(out) + strlen(decl) + 1);
                if (!merged) {
                    free(decl);
                    free(out);
                    return NULL;
                }
                memcpy(merged, out, before);
                strcpy(merged + before, decl);
                strcat(merged + before + strlen(decl), out + before);
                free(decl);
                free(out);
                out = merged;
            } else {
                char *merged = (char *)malloc(strlen(out) + strlen(decl) + 1);
                if (!merged) {
                    free(decl);
                    free(out);
                    return NULL;
                }
                strcpy(merged, out);
                strcat(merged, decl);
                free(decl);
                free(out);
                out = merged;
            }
            out = replace_all_ident(out, "gl_FragData[0]", "fsgl_FragColor");
            out = replace_all_ident(out, "gl_FragColor", "fsgl_FragColor");
        }
    }

    if (changed) {
        *changed = true;
    }
    return out;
}
