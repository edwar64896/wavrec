#include "ixml.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * iXML v2.0 chunk renderer.
 *
 * Produces an XML document intended for the WAV 'iXML' chunk.  Includes the
 * minimum set of fields that professional DAWs / editors (Reaper, Pro Tools,
 * Nuendo, Premiere, Davinci Resolve) consume for:
 *   - PROJECT / SCENE / TAKE / TAPE
 *   - TRACK_LIST with per-channel NAME (this is what makes channel 3 show up
 *     as "Boom" instead of "track 3" on import)
 *   - SPEED section with timecode rate / flag / sample rate
 *   - BWF metadata mirrors so tools that only read iXML still get them
 *
 * Reference: iXML specification v2.0 (www.ixml.info).
 * ---------------------------------------------------------------------- */

/* Append-only XML buffer.  Single allocation, grows as needed. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} XmlBuf;

static void xb_init(XmlBuf *b)
{
    b->cap = 4096;
    b->len = 0;
    b->buf = (char *)malloc(b->cap);
    if (b->buf) b->buf[0] = '\0';
}

static void xb_reserve(XmlBuf *b, size_t extra)
{
    if (!b->buf) return;
    if (b->len + extra + 1 > b->cap) {
        size_t new_cap = b->cap * 2;
        while (b->len + extra + 1 > new_cap) new_cap *= 2;
        char *nb = (char *)realloc(b->buf, new_cap);
        if (!nb) return;
        b->buf = nb;
        b->cap = new_cap;
    }
}

static void xb_raw(XmlBuf *b, const char *s)
{
    if (!b->buf || !s) return;
    size_t n = strlen(s);
    xb_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void xb_printf(XmlBuf *b, const char *fmt, ...)
{
    if (!b->buf) return;
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        size_t remaining = b->cap - b->len;
        int wrote = vsnprintf(b->buf + b->len, remaining, fmt, ap);
        va_end(ap);
        if (wrote < 0) return;
        if ((size_t)wrote < remaining) { b->len += (size_t)wrote; return; }
        xb_reserve(b, (size_t)wrote + 1);
        if (!b->buf) return;
    }
}

/* XML-escape s into b: &, <, >, ", ' → entities. */
static void xb_escape(XmlBuf *b, const char *s)
{
    if (!b->buf || !s) return;
    for (; *s; s++) {
        const char *r = NULL;
        switch (*s) {
            case '&':  r = "&amp;";  break;
            case '<':  r = "&lt;";   break;
            case '>':  r = "&gt;";   break;
            case '"':  r = "&quot;"; break;
            case '\'': r = "&apos;"; break;
            default:   break;
        }
        if (r) { xb_raw(b, r); }
        else   {
            xb_reserve(b, 1);
            b->buf[b->len++] = *s;
            b->buf[b->len]   = '\0';
        }
    }
}

/* Convenience: <TAG>escaped value</TAG>\n.  Emits empty element if val is NULL/"". */
static void xb_tag(XmlBuf *b, const char *indent, const char *tag, const char *val)
{
    if (!val || !*val) {
        xb_printf(b, "%s<%s></%s>\n", indent, tag, tag);
        return;
    }
    xb_printf(b, "%s<%s>", indent, tag);
    xb_escape(b, val);
    xb_printf(b, "</%s>\n", tag);
}

/* -------------------------------------------------------------------------
 * Timecode rate → iXML fraction and flag strings.
 *
 * iXML v2.0 TIMECODE_RATE is a rational "num/den" — e.g. 24000/1001 for
 * 23.976, 25/1 for PAL.  TIMECODE_FLAG is "DF" or "NDF".
 * ---------------------------------------------------------------------- */

static void tc_rate_strings(const WavRecTimecodeSource *tc,
                            char *rate_out, size_t rate_cap,
                            const char **flag_out)
{
    if (!tc) {
        snprintf(rate_out, rate_cap, "25/1");
        *flag_out = "NDF";
        return;
    }
    const TcRateInfo *ri = tc_rate_info(tc->rate);
    if (!ri) {
        snprintf(rate_out, rate_cap, "25/1");
        *flag_out = "NDF";
        return;
    }
    snprintf(rate_out, rate_cap, "%u/%u", ri->num, ri->den);
    *flag_out = ri->drop_frame ? "DF" : "NDF";
}

/* -------------------------------------------------------------------------
 * Public: render to a malloc'd null-terminated string.
 * ---------------------------------------------------------------------- */

char *ixml_render(const IxmlParams *p)
{
    XmlBuf b; xb_init(&b);
    if (!b.buf) return NULL;

    char rate_str[32]; const char *tc_flag;
    tc_rate_strings(p ? p->tc : NULL, rate_str, sizeof(rate_str), &tc_flag);

    xb_raw(&b,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<BWFXML>\n"
        "  <IXML_VERSION>2.0</IXML_VERSION>\n");

    xb_tag(&b, "  ", "PROJECT", p ? p->project : NULL);
    xb_tag(&b, "  ", "SCENE",   p ? p->scene   : NULL);
    xb_tag(&b, "  ", "TAKE",    p ? p->take    : NULL);
    xb_tag(&b, "  ", "TAPE",    p ? p->tape    : NULL);

    /* SPEED section — timecode + sample rate descriptors. */
    xb_raw(&b, "  <SPEED>\n");
    xb_printf(&b, "    <NOTE>WavRec</NOTE>\n");
    xb_printf(&b, "    <MASTER_SPEED>%s</MASTER_SPEED>\n", rate_str);
    xb_printf(&b, "    <CURRENT_SPEED>%s</CURRENT_SPEED>\n", rate_str);
    xb_printf(&b, "    <TIMECODE_RATE>%s</TIMECODE_RATE>\n", rate_str);
    xb_printf(&b, "    <TIMECODE_FLAG>%s</TIMECODE_FLAG>\n", tc_flag);
    xb_printf(&b, "    <FILE_SAMPLE_RATE>%u</FILE_SAMPLE_RATE>\n",
                    p ? p->sample_rate : 48000);
    xb_printf(&b, "    <AUDIO_BIT_DEPTH>%u</AUDIO_BIT_DEPTH>\n",
                    p ? p->bit_depth : 24);
    xb_raw(&b, "  </SPEED>\n");

    /* TRACK_LIST — the thing the user actually asked for.
     * iXML TRACK entries are 1-based in CHANNEL_INDEX / INTERLEAVE_INDEX. */
    int n = (p && p->n_tracks > 0) ? p->n_tracks : 0;
    xb_raw(&b, "  <TRACK_LIST>\n");
    xb_printf(&b, "    <TRACK_COUNT>%d</TRACK_COUNT>\n", n);
    for (int i = 0; i < n; i++) {
        const char *name = (p->track_names && p->track_names[i])
                            ? p->track_names[i] : "";
        uint8_t hw = (p->track_hw_inputs) ? p->track_hw_inputs[i] : 0;
        xb_raw   (&b, "    <TRACK>\n");
        xb_printf(&b, "      <CHANNEL_INDEX>%d</CHANNEL_INDEX>\n", i + 1);
        xb_printf(&b, "      <INTERLEAVE_INDEX>%d</INTERLEAVE_INDEX>\n", i + 1);
        xb_printf(&b, "      <NAME>");
        xb_escape(&b, name);
        xb_raw   (&b, "</NAME>\n");
        xb_printf(&b, "      <FUNCTION>input %u</FUNCTION>\n", (unsigned)(hw + 1));
        xb_raw   (&b, "    </TRACK>\n");
    }
    xb_raw(&b, "  </TRACK_LIST>\n");

    xb_raw(&b, "</BWFXML>\n");
    return b.buf; /* caller frees */
}
