#include <gtk/gtk.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#define API_LEADS     "http://localhost:8080/api/leads"
#define API_TASKS     "http://localhost:8080/api/all-tasks"
#define REFRESH_MS    120000
#define SOCK_PATH     "/tmp/cxm-control.sock"

/* ── HTTP ───────────────────────────────────────────── */
typedef struct { char *buf; size_t len; } Chunk;

static size_t write_cb(char *p, size_t sz, size_t n, void *ud) {
    Chunk *c = (Chunk *)ud;
    size_t add = sz * n;
    c->buf = realloc(c->buf, c->len + add + 1);
    memcpy(c->buf + c->len, p, add);
    c->len += add;
    c->buf[c->len] = 0;
    return add;
}

static char *http_get(const char *url) {
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    Chunk c = { malloc(1), 0 };
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);
    CURLcode r = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (r != CURLE_OK) { free(c.buf); return NULL; }
    return c.buf;
}

/* POST with optional JSON body. Returns response body (caller frees) or NULL. */
static char *http_post(const char *url, const char *json_body) {
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    Chunk c = { malloc(1), 0 };
    struct curl_slist *hdrs = NULL;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    if (json_body) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, json_body);
    } else {
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, "");
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 25L);
    CURLcode r = curl_easy_perform(h);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    if (r != CURLE_OK) { free(c.buf); return NULL; }
    return c.buf;
}

/* ── State ──────────────────────────────────────────── */
#define N_LEAD_TABS 4
static const char *PRIORITIES[N_LEAD_TABS] = { "A", "B", "C", "" };
static const char *TAB_NAMES[5] = {
    "Priority A", "Priority B", "Priority C", "Unprioritized", "Tasks"
};

static GtkWidget *notebook;
static GtkWidget *flow_boxes[N_LEAD_TABS];
static GtkWidget *task_list_box;
static GtkWidget *lbl_counts[5];
static GtkWidget *lbl_clock;
static GtkWidget *entry_search;
static char       search_q[256] = "";
static char      *cached_leads  = NULL;
static char      *cached_tasks  = NULL;

/* ── CSS ─────────────────────────────────────────────── */
static const char *APP_CSS =
    "window { background-color: #0d0f18; }"
    "scrolledwindow, viewport, flowbox { background-color: #0d0f18; }"
    "flowboxchild { background-color: transparent; padding: 0; outline: none; }"
    "flowboxchild:focus { outline: none; }"
    "notebook { background-color: #0d0f18; }"
    "notebook header { background-color: #13151f; border-bottom: 1px solid #252840; }"
    "notebook tab { background-color: #13151f; color: #64748b;"
    "  padding: 5px 14px; border: none; }"
    "notebook tab:checked { background-color: #1a1d2e; color: #e2e8f0;"
    "  border-bottom: 2px solid #6366f1; }"
    ".hdr { background-color: #13151f; border-bottom: 1px solid #252840; }"
    ".title { font-size: 16.5pt; font-weight: bold; color: #6366f1; }"
    ".clock { font-size: 12pt; color: #4b5563; }"
    ".count { font-size: 12pt; color: #475569; }"
    ".card-frame { background-color: #1a1d2e; border-radius: 5px;"
    "  border: 1px solid #252840; }"
    ".card-name  { font-size: 14pt; font-weight: bold; color: #f1f5f9; }"
    ".card-type  { font-size: 10.5pt;   font-weight: bold; }"
    ".card-agent { font-size: 11pt; color: #4b5563; }"
    ".card-added { font-size: 12pt;   color: #9ca3af; }"
    ".card-step  { font-size: 11pt; color: #94a3b8; font-style: italic; }"
    ".type-hot     { color: #fca5a5; }"
    ".type-listing { color: #c4b5fd; }"
    ".type-buyer   { color: #86efac; }"
    ".type-general { color: #7dd3fc; }"
    /* Task list rows */
    ".task-row { background-color: #13151f; border-bottom: 1px solid #1e2030; }"
    ".task-row:hover { background-color: #1a1d2e; }"
    ".task-title    { font-size: 13.5pt; font-weight: bold; color: #e2e8f0; }"
    ".task-lead     { font-size: 11pt; color: #6366f1; }"
    ".task-agent    { font-size: 11pt; color: #4b5563; }"
    ".task-due      { font-size: 11pt; color: #f97316; }"
    ".task-due-ok   { font-size: 11pt; color: #374151; }"
    ".task-done     { font-size: 11pt; color: #374151; }"
    ".status-pending    { color: #f97316; font-size: 10.5pt; font-weight: bold; }"
    ".status-inprogress { color: #6366f1; font-size: 10.5pt; font-weight: bold; }"
    ".status-done       { color: #374151; font-size: 10.5pt; }"
    /* Card action buttons */
    ".act-complete, .act-archive { font-size: 10pt; font-weight: bold;"
    "  padding: 3px 6px; border-radius: 6px; border: 1px solid #252840;"
    "  background-image: none; background-color: #0d0f18; color: #7681a0;"
    "  text-shadow: none; box-shadow: none; }"
    ".act-complete:hover { background-color: #11241a; color: #86efac; border-color: #1d5e36; }"
    ".act-archive:hover  { background-color: #241c0e; color: #fcd34d; border-color: #6b4d12; }"
    /* Search */
    "searchentry { background-color: #13151f; color: #e2e8f0;"
    "  border: 1px solid #252840; border-radius: 4px; }";

/* ── Helpers ─────────────────────────────────────────── */
static void fmt_date(const char *iso, char *out, size_t n) {
    if (!iso || !*iso) { *out = 0; return; }
    struct tm t = {0};
    sscanf(iso, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    t.tm_year -= 1900; t.tm_mon -= 1;
    strftime(out, n, "%b %e, %Y", &t);
}

static int is_overdue(const char *due_iso) {
    if (!due_iso || !*due_iso) return 0;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", lt);
    return strncmp(due_iso, today, 10) < 0;
}

/* ── Color bar draw ──────────────────────────────────── */
static gboolean draw_bar(GtkWidget *w, cairo_t *cr, gpointer ud) {
    (void)ud;
    const char *col = gtk_widget_get_name(w);
    guint r = 0, g = 0, b = 0;
    sscanf(col, "#%02x%02x%02x", &r, &g, &b);
    cairo_set_source_rgb(cr, r/255.0, g/255.0, b/255.0);
    cairo_paint(cr);
    return FALSE;
}

static const char *priority_color(const char *pri) {
    if (!pri || !*pri)        return "#6b7280";
    if (strcmp(pri,"A") == 0) return "#ef4444";
    if (strcmp(pri,"B") == 0) return "#f97316";
    if (strcmp(pri,"C") == 0) return "#3b82f6";
    return "#6b7280";
}

/* ── Card actions: Complete / Archive ────────────────── */
static gboolean refresh_all(gpointer d);   /* forward decl */

#define API_BASE "http://localhost:8080/api/leads"

typedef struct { char *id; char *name; } LeadRef;

static void leadref_free(gpointer p) {
    LeadRef *lr = p;
    if (!lr) return;
    g_free(lr->id); g_free(lr->name); g_free(lr);
}

static void flash_toast(GtkWidget *parent, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(parent),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_complete_clicked(GtkButton *b, gpointer ud) {
    LeadRef *lr = ud;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(top),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Complete \"%s\"?\nPatrick will be emailed so he can follow up.", lr->name);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "Cancel", GTK_RESPONSE_CANCEL, "\xE2\x9C\x93 Complete", GTK_RESPONSE_OK, NULL);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) return;

    char url[256];
    snprintf(url, sizeof(url), "%s/%s/complete", API_BASE, lr->id);
    char *r = http_post(url, NULL);
    int ok = 0, emailed = 0;
    if (r) {
        json_object *resp = json_tokener_parse(r);
        if (resp && json_object_is_type(resp, json_type_object)) {
            json_object *o;
            ok = json_object_object_get_ex(resp, "lead", &o);
            if (json_object_object_get_ex(resp, "emailSent", &o))
                emailed = json_object_get_boolean(o);
        }
        if (resp) json_object_put(resp);
        free(r);
    }
    refresh_all(NULL);
    if (ok) flash_toast(top, emailed ? "Lead completed — Patrick emailed."
                                     : "Lead completed. (Email skipped — add RESEND_API_KEY.)");
    else    flash_toast(top, "Failed to complete lead — check the connection.");
}

static void on_archive_clicked(GtkButton *b, gpointer ud) {
    LeadRef *lr = ud;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(top),
        GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Archive \"%s\"?\nWhy is it being archived?", lr->name);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "Cancel", GTK_RESPONSE_CANCEL,
        "Expired", 1, "Lost", 2, "Other", 3, NULL);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    const char *reason = NULL;
    if (resp == 1) reason = "expired";
    else if (resp == 2) reason = "cancelled";
    else if (resp == 3) reason = "other";
    if (!reason) return;

    char url[256], body[64];
    snprintf(url, sizeof(url), "%s/%s/archive", API_BASE, lr->id);
    snprintf(body, sizeof(body), "{\"reason\":\"%s\"}", reason);
    char *r = http_post(url, body);
    int ok = (r != NULL);
    free(r);
    refresh_all(NULL);
    if (!ok) flash_toast(top, "Failed to archive lead — check the connection.");
}

/* ── Lead card ───────────────────────────────────────── */
static GtkWidget *make_card(json_object *lead) {
    const char *name  = json_object_get_string(json_object_object_get(lead,"name"))     ?: "—";
    const char *type  = json_object_get_string(json_object_object_get(lead,"type"))     ?: "";
    const char *agent = json_object_get_string(json_object_object_get(lead,"agent"))    ?: "";
    const char *step  = json_object_get_string(json_object_object_get(lead,"nextStep")) ?: "";
    const char *cre   = json_object_get_string(json_object_object_get(lead,"createdAt"))?:"";
    const char *pri   = json_object_get_string(json_object_object_get(lead,"priority")) ?: "";
    const char *id    = json_object_get_string(json_object_object_get(lead,"id"))       ?: "";
    if (strcmp(step,"None") == 0) step = "";

    char date_str[32]; fmt_date(cre, date_str, sizeof(date_str));

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "card-frame");
    gtk_widget_set_hexpand(frame, TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(frame), hbox);

    /* Color bar */
    GtkWidget *bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(bar, 4, -1);
    gtk_widget_set_name(bar, priority_color(pri));
    g_signal_connect(bar, "draw", G_CALLBACK(draw_bar), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), bar, FALSE, FALSE, 0);

    /* Content */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(vbox, 6);
    gtk_widget_set_margin_end(vbox, 6);
    gtk_widget_set_margin_top(vbox, 5);
    gtk_widget_set_margin_bottom(vbox, 5);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    /* Name + type */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lname = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(lname), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(lname), 0.0);
    gtk_widget_set_hexpand(lname, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(lname), "card-name");
    gtk_box_pack_start(GTK_BOX(row1), lname, TRUE, TRUE, 0);
    if (*type) {
        GtkWidget *lt = gtk_label_new(type);
        gtk_style_context_add_class(gtk_widget_get_style_context(lt), "card-type");
        const char *tc = "type-general";
        if (strcasecmp(type,"Hot")==0)     tc = "type-hot";
        else if (strcasecmp(type,"Listing")==0) tc = "type-listing";
        else if (strcasecmp(type,"Buyer")==0)   tc = "type-buyer";
        gtk_style_context_add_class(gtk_widget_get_style_context(lt), tc);
        gtk_box_pack_end(GTK_BOX(row1), lt, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), row1, FALSE, FALSE, 0);

    if (*agent) {
        char buf[256]; snprintf(buf,sizeof(buf),"👤 %s",agent);
        GtkWidget *la = gtk_label_new(buf);
        gtk_label_set_xalign(GTK_LABEL(la),0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(la),"card-agent");
        gtk_box_pack_start(GTK_BOX(vbox),la,FALSE,FALSE,0);
    }
    if (*date_str) {
        char buf[64]; snprintf(buf,sizeof(buf),"Added %s",date_str);
        GtkWidget *ld = gtk_label_new(buf);
        gtk_label_set_xalign(GTK_LABEL(ld),0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(ld),"card-added");
        gtk_box_pack_start(GTK_BOX(vbox),ld,FALSE,FALSE,0);
    }
    if (*step) {
        char buf[300]; snprintf(buf,sizeof(buf),"→ %s",step);
        GtkWidget *ls = gtk_label_new(buf);
        gtk_label_set_ellipsize(GTK_LABEL(ls),PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(ls),0.0);
        gtk_style_context_add_class(gtk_widget_get_style_context(ls),"card-step");
        gtk_box_pack_start(GTK_BOX(vbox),ls,FALSE,FALSE,0);
    }

    /* Action buttons: Complete / Archive */
    if (*id) {
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_top(actions, 4);

        GtkWidget *bc = gtk_button_new_with_label("\xE2\x9C\x93 Complete");
        gtk_style_context_add_class(gtk_widget_get_style_context(bc), "act-complete");
        LeadRef *lr1 = g_malloc(sizeof(LeadRef));
        lr1->id = g_strdup(id); lr1->name = g_strdup(name);
        g_object_set_data_full(G_OBJECT(bc), "lead", lr1, leadref_free);
        g_signal_connect(bc, "clicked", G_CALLBACK(on_complete_clicked), lr1);
        gtk_box_pack_start(GTK_BOX(actions), bc, TRUE, TRUE, 0);

        GtkWidget *ba = gtk_button_new_with_label("\xF0\x9F\x97\x84 Archive");
        gtk_style_context_add_class(gtk_widget_get_style_context(ba), "act-archive");
        LeadRef *lr2 = g_malloc(sizeof(LeadRef));
        lr2->id = g_strdup(id); lr2->name = g_strdup(name);
        g_object_set_data_full(G_OBJECT(ba), "lead", lr2, leadref_free);
        g_signal_connect(ba, "clicked", G_CALLBACK(on_archive_clicked), lr2);
        gtk_box_pack_start(GTK_BOX(actions), ba, TRUE, TRUE, 0);

        gtk_box_pack_start(GTK_BOX(vbox), actions, FALSE, FALSE, 0);
    }
    return frame;
}

/* ── Task row ────────────────────────────────────────── */
static GtkWidget *make_task_row(json_object *task) {
    const char *title  = json_object_get_string(json_object_object_get(task,"title"))    ?: "—";
    const char *lead   = json_object_get_string(json_object_object_get(task,"leadName")) ?: "";
    const char *agent  = json_object_get_string(json_object_object_get(task,"leadAgent"))?:"";
    const char *status = json_object_get_string(json_object_object_get(task,"status"))   ?: "pending";
    const char *due    = json_object_get_string(json_object_object_get(task,"dueDate"))  ?: "";
    const char *pri    = json_object_get_string(json_object_object_get(task,"leadPriority"))?:"";

    char due_str[32]; fmt_date(due, due_str, sizeof(due_str));

    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row),"task-row");

    /* Horizontal: color bar + content */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(row), hbox);

    GtkWidget *bar = gtk_drawing_area_new();
    gtk_widget_set_size_request(bar, 4, -1);
    gtk_widget_set_name(bar, priority_color(pri));
    g_signal_connect(bar, "draw", G_CALLBACK(draw_bar), NULL);
    gtk_box_pack_start(GTK_BOX(hbox), bar, FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 5);
    gtk_widget_set_margin_bottom(vbox, 5);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    /* Title + status */
    GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lt = gtk_label_new(title);
    gtk_label_set_ellipsize(GTK_LABEL(lt), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(lt), 0.0);
    gtk_widget_set_hexpand(lt, TRUE);
    int done = (strcmp(status,"done")==0);
    gtk_style_context_add_class(gtk_widget_get_style_context(lt), done ? "task-done" : "task-title");
    gtk_box_pack_start(GTK_BOX(r1), lt, TRUE, TRUE, 0);

    const char *status_lbl = "PENDING";
    const char *status_cls = "status-pending";
    if (strcmp(status,"in_progress")==0) { status_lbl="IN PROGRESS"; status_cls="status-inprogress"; }
    else if (strcmp(status,"done")==0)   { status_lbl="DONE";        status_cls="status-done"; }
    else if (strcmp(status,"cancelled")==0){ status_lbl="CANCELLED"; status_cls="status-done"; }
    GtkWidget *ls = gtk_label_new(status_lbl);
    gtk_style_context_add_class(gtk_widget_get_style_context(ls), status_cls);
    gtk_box_pack_end(GTK_BOX(r1), ls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), r1, FALSE, FALSE, 0);

    /* Lead + agent + due */
    GtkWidget *r2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    if (*lead) {
        GtkWidget *ll = gtk_label_new(lead);
        gtk_label_set_ellipsize(GTK_LABEL(ll), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(ll), 0.0);
        gtk_widget_set_hexpand(ll, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(ll),"task-lead");
        gtk_box_pack_start(GTK_BOX(r2), ll, TRUE, TRUE, 0);
    }
    if (*agent) {
        char buf[128]; snprintf(buf,sizeof(buf),"👤 %s",agent);
        GtkWidget *la = gtk_label_new(buf);
        gtk_style_context_add_class(gtk_widget_get_style_context(la),"task-agent");
        gtk_box_pack_start(GTK_BOX(r2), la, FALSE, FALSE, 0);
    }
    if (*due_str) {
        char buf[48]; snprintf(buf,sizeof(buf),"Due %s",due_str);
        GtkWidget *ld = gtk_label_new(buf);
        int over = is_overdue(due) && strcmp(status,"done")!=0;
        gtk_style_context_add_class(gtk_widget_get_style_context(ld), over ? "task-due" : "task-due-ok");
        gtk_box_pack_end(GTK_BOX(r2), ld, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), r2, FALSE, FALSE, 0);

    return row;
}

/* ── Populate lead tab ───────────────────────────────── */
static void populate_leads(int tab_idx, const char *json_str, const char *query) {
    GtkWidget *fb = flow_boxes[tab_idx];
    const char *pri = PRIORITIES[tab_idx];

    GList *kids = gtk_container_get_children(GTK_CONTAINER(fb));
    for (GList *l = kids; l; l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    if (!json_str) { gtk_label_set_text(GTK_LABEL(lbl_counts[tab_idx]),"Error"); return; }

    json_object *arr = json_tokener_parse(json_str);
    if (!arr || !json_object_is_type(arr, json_type_array)) { if (arr) json_object_put(arr); return; }

    int n = json_object_array_length(arr), shown = 0, total = 0;
    for (int i = 0; i < n; i++) {
        json_object *lead = json_object_array_get_idx(arr, i);
        json_object *ao, *po;
        json_object_object_get_ex(lead,"archived",&ao);
        json_object_object_get_ex(lead,"priority",&po);
        if (ao && json_object_get_boolean(ao)) continue;
        const char *p = po ? json_object_get_string(po) : "";
        if (strcmp(p, pri) != 0) continue;
        total++;
        if (query && *query) {
            const char *nm = json_object_get_string(json_object_object_get(lead,"name"))    ?: "";
            const char *ag = json_object_get_string(json_object_object_get(lead,"agent"))   ?: "";
            const char *st = json_object_get_string(json_object_object_get(lead,"nextStep"))?:"";
            char hay[512], ql[256];
            snprintf(hay,sizeof(hay),"%s %s %s",nm,ag,st);
            for(size_t j=0;j<strlen(hay);j++) hay[j]=tolower((unsigned char)hay[j]);
            strncpy(ql,query,sizeof(ql)-1); ql[sizeof(ql)-1]=0;
            for(size_t j=0;j<strlen(ql);j++) ql[j]=tolower((unsigned char)ql[j]);
            if (!strstr(hay,ql)) continue;
        }
        GtkWidget *card = make_card(lead);
        gtk_flow_box_insert(GTK_FLOW_BOX(fb), card, -1);
        gtk_widget_show_all(card);
        shown++;
    }
    json_object_put(arr);

    char buf[64];
    if (query && *query) snprintf(buf,sizeof(buf),"%d / %d",shown,total);
    else                 snprintf(buf,sizeof(buf),"%d leads",total);
    gtk_label_set_text(GTK_LABEL(lbl_counts[tab_idx]), buf);
}

/* ── Populate tasks tab ──────────────────────────────── */
static void populate_tasks(const char *json_str, const char *query) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(task_list_box));
    for (GList *l = kids; l; l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    if (!json_str) { gtk_label_set_text(GTK_LABEL(lbl_counts[4]),"Error"); return; }

    json_object *arr = json_tokener_parse(json_str);
    if (!arr || !json_object_is_type(arr, json_type_array)) { if (arr) json_object_put(arr); return; }

    int n = json_object_array_length(arr), shown = 0;
    for (int i = 0; i < n; i++) {
        json_object *task = json_object_array_get_idx(arr, i);
        if (query && *query) {
            const char *ti = json_object_get_string(json_object_object_get(task,"title"))   ?: "";
            const char *ln = json_object_get_string(json_object_object_get(task,"leadName"))?:"";
            char hay[512], ql[256];
            snprintf(hay,sizeof(hay),"%s %s",ti,ln);
            for(size_t j=0;j<strlen(hay);j++) hay[j]=tolower((unsigned char)hay[j]);
            strncpy(ql,query,sizeof(ql)-1); ql[sizeof(ql)-1]=0;
            for(size_t j=0;j<strlen(ql);j++) ql[j]=tolower((unsigned char)ql[j]);
            if (!strstr(hay,ql)) continue;
        }
        GtkWidget *row = make_task_row(task);
        gtk_list_box_insert(GTK_LIST_BOX(task_list_box), row, -1);
        gtk_widget_show_all(row);
        shown++;
    }
    json_object_put(arr);

    char buf[64]; snprintf(buf,sizeof(buf),"%d tasks",shown);
    gtk_label_set_text(GTK_LABEL(lbl_counts[4]), buf);
}

/* ── Refresh all data ────────────────────────────────── */
static gboolean refresh_all(gpointer d) {
    (void)d;
    char *leads = http_get(API_LEADS);
    if (leads) { free(cached_leads); cached_leads = leads; }
    for (int i = 0; i < N_LEAD_TABS; i++)
        populate_leads(i, cached_leads, search_q);

    char *tasks = http_get(API_TASKS);
    if (tasks) { free(cached_tasks); cached_tasks = tasks; }
    populate_tasks(cached_tasks, search_q);

    return G_SOURCE_CONTINUE;
}

/* ── Search ──────────────────────────────────────────── */
static void on_search(GtkSearchEntry *e, gpointer d) {
    (void)d;
    strncpy(search_q, gtk_entry_get_text(GTK_ENTRY(e)), sizeof(search_q)-1);
    for (int i = 0; i < N_LEAD_TABS; i++)
        populate_leads(i, cached_leads, search_q);
    populate_tasks(cached_tasks, search_q);
}

/* ── Clock ───────────────────────────────────────────── */
static gboolean tick_clock(gpointer d) {
    (void)d;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%d  %H:%M",tm);
    gtk_label_set_text(GTK_LABEL(lbl_clock), buf);
    return G_SOURCE_CONTINUE;
}

/* ── Tab label with count ────────────────────────────── */
static GtkWidget *make_tab_label(int idx) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *lname = gtk_label_new(TAB_NAMES[idx]);
    lbl_counts[idx] = gtk_label_new("…");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_counts[idx]), "count");
    gtk_box_pack_start(GTK_BOX(box), lname, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl_counts[idx], FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

/* ── Main ────────────────────────────────────────────── */

/* socket control */
static gboolean on_socket_data(GIOChannel *ch, GIOCondition cond, gpointer d) {
    (void)d; (void)cond;
    int fd = g_io_channel_unix_get_fd(ch);
    int client = accept(fd, NULL, NULL);
    if (client < 0) return G_SOURCE_CONTINUE;
    char buf[256] = {0};
    ssize_t n = read(client, buf, sizeof(buf)-1);
    close(client);
    if (n <= 0) return G_SOURCE_CONTINUE;
    buf[n] = 0;
    for (int i = 0; i < n; i++) if (buf[i] == 10) buf[i] = 0;
    if (strncmp(buf, "tab ", 4) == 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), atoi(buf + 4));
    } else if (strncmp(buf, "search ", 7) == 0) {
        gtk_entry_set_text(GTK_ENTRY(entry_search), buf + 7);
        g_signal_emit_by_name(entry_search, "search-changed");
    } else if (strcmp(buf, "clear") == 0) {
        gtk_entry_set_text(GTK_ENTRY(entry_search), "");
        g_signal_emit_by_name(entry_search, "search-changed");
    } else if (strcmp(buf, "refresh") == 0) {
        refresh_all(NULL);
    }
    return G_SOURCE_CONTINUE;
}
static void setup_control_socket(void) {
    unlink(SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return; }
    listen(fd, 4);
    GIOChannel *ch = g_io_channel_unix_new(fd);
    g_io_add_watch(ch, G_IO_IN, on_socket_data, NULL);
    g_io_channel_unref(ch);
}
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, APP_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "CXM Leads");
    gtk_window_maximize(GTK_WINDOW(win));
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), root);

    /* Header */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hdr, 10);
    gtk_widget_set_margin_end(hdr, 10);
    gtk_widget_set_margin_top(hdr, 5);
    gtk_widget_set_margin_bottom(hdr, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(hdr), "hdr");
    gtk_box_pack_start(GTK_BOX(root), hdr, FALSE, FALSE, 0);

    GtkWidget *ltitle = gtk_label_new("⚡ CXM Leads");
    gtk_style_context_add_class(gtk_widget_get_style_context(ltitle), "title");
    gtk_box_pack_start(GTK_BOX(hdr), ltitle, FALSE, FALSE, 0);

    entry_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_search), "Search…");
    gtk_widget_set_size_request(entry_search, 200, -1);
    g_signal_connect(entry_search, "search-changed", G_CALLBACK(on_search), NULL);
    gtk_box_pack_start(GTK_BOX(hdr), entry_search, FALSE, FALSE, 0);

    lbl_clock = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_clock), "clock");
    gtk_box_pack_end(GTK_BOX(hdr), lbl_clock, FALSE, FALSE, 0);

    /* Notebook */
    notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(root), notebook, TRUE, TRUE, 0);

    /* Lead tabs (A, B, C, Unprioritized) */
    for (int i = 0; i < N_LEAD_TABS; i++) {
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
            GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

        flow_boxes[i] = gtk_flow_box_new();
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_boxes[i]), 4);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow_boxes[i]), 4);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_boxes[i]), 6);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_boxes[i]), 6);
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_boxes[i]), GTK_SELECTION_NONE);
        gtk_widget_set_margin_start(flow_boxes[i], 8);
        gtk_widget_set_margin_end(flow_boxes[i], 8);
        gtk_widget_set_margin_top(flow_boxes[i], 6);
        gtk_widget_set_margin_bottom(flow_boxes[i], 6);
        gtk_container_add(GTK_CONTAINER(sw), flow_boxes[i]);

        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sw, make_tab_label(i));
    }

    /* Tasks tab */
    GtkWidget *tsw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tsw),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    task_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(task_list_box), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(tsw), task_list_box);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tsw, make_tab_label(4));

    gtk_widget_show_all(win);

    tick_clock(NULL);
    g_timeout_add(1000, tick_clock, NULL);
    refresh_all(NULL);
    g_timeout_add(REFRESH_MS, refresh_all, NULL);

    setup_control_socket();
    gtk_main();
    curl_global_cleanup();
    return 0;
}
