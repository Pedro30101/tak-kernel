#include "tak.h"
#include "py.h"

#define MAXV 128
#define MAXL 256
#define MAXSTR 128

enum { V_INT = 1, V_STR = 2 };

struct val {
    int t;
    i64 num;
    char str[MAXSTR];
};

struct var {
    char name[32];
    struct val v;
};

struct env {
    struct var vars[MAXV];
    int nvar;
    char *out;
    int max, pos;
};

static void emit(struct env *e, const char *s) {
    while (*s && e->pos < e->max - 1) {
        e->out[e->pos++] = *s++;
    }
    e->out[e->pos] = 0;
}

static void emit_num(struct env *e, i64 n) {
    char b[32];
    snprintf(b, sizeof b, "%ld", (long)n);
    emit(e, b);
}

static struct var *find_var(struct env *e, const char *name) {
    for (int i = 0; i < e->nvar; i++)
        if (!strcmp(e->vars[i].name, name)) return &e->vars[i];
    return NULL;
}

static void set_var_int(struct env *e, const char *name, i64 val) {
    struct var *v = find_var(e, name);
    if (!v) {
        if (e->nvar >= MAXV) return;
        v = &e->vars[e->nvar++];
        strncpy(v->name, name, 31);
    }
    v->v.t = V_INT;
    v->v.num = val;
    v->v.str[0] = 0;
}

static void set_var_str(struct env *e, const char *name, const char *val) {
    struct var *v = find_var(e, name);
    if (!v) {
        if (e->nvar >= MAXV) return;
        v = &e->vars[e->nvar++];
        strncpy(v->name, name, 31);
    }
    v->v.t = V_STR;
    v->v.num = 0;
    strncpy(v->v.str, val, MAXSTR - 1);
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static struct val eval_expr(struct env *e, const char **sp);

static struct val eval_atom(struct env *e, const char **sp) {
    const char *p = skip_ws(*sp);
    struct val res = { V_INT, 0, "" };
    if (!*p) { *sp = p; return res; }
    if (*p == '(') {
        p++;
        res = eval_expr(e, &p);
        p = skip_ws(p);
        if (*p == ')') p++;
        *sp = p;
        return res;
    }
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        int idx = 0;
        while (*p && *p != q && idx < MAXSTR - 1) {
            if (*p == '\\' && *(p + 1)) {
                p++;
                if (*p == 'n') res.str[idx++] = '\n';
                else if (*p == 't') res.str[idx++] = '\t';
                else res.str[idx++] = *p;
                p++;
            } else {
                res.str[idx++] = *p++;
            }
        }
        if (*p == q) p++;
        res.str[idx] = 0;
        res.t = V_STR;
        *sp = p;
        return res;
    }
    if ((*p >= '0' && *p <= '9') || (*p == '-' && *(p + 1) >= '0' && *(p + 1) <= '9')) {
        int sgn = 1;
        if (*p == '-') { sgn = -1; p++; }
        i64 n = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p - '0');
            p++;
        }
        res.t = V_INT;
        res.num = n * sgn;
        *sp = p;
        return res;
    }
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        char name[32];
        int ni = 0;
        while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '_') && ni < 31) {
            name[ni++] = *p++;
        }
        name[ni] = 0;
        if (!strcmp(name, "True")) { res.t = V_INT; res.num = 1; *sp = p; return res; }
        if (!strcmp(name, "False")) { res.t = V_INT; res.num = 0; *sp = p; return res; }
        struct var *v = find_var(e, name);
        if (v) res = v->v;
        *sp = p;
        return res;
    }
    *sp = p;
    return res;
}

static struct val eval_term(struct env *e, const char **sp) {
    struct val left = eval_atom(e, sp);
    for (;;) {
        const char *p = skip_ws(*sp);
        if (*p == '*' || *p == '/' || *p == '%') {
            char op = *p++;
            *sp = p;
            struct val right = eval_atom(e, sp);
            if (left.t == V_INT && right.t == V_INT) {
                if (op == '*') left.num *= right.num;
                else if (op == '/' && right.num) left.num /= right.num;
                else if (op == '%' && right.num) left.num %= right.num;
            }
        } else break;
    }
    return left;
}

static struct val eval_add(struct env *e, const char **sp) {
    struct val left = eval_term(e, sp);
    for (;;) {
        const char *p = skip_ws(*sp);
        if (*p == '+' || *p == '-') {
            char op = *p++;
            *sp = p;
            struct val right = eval_term(e, sp);
            if (left.t == V_INT && right.t == V_INT) {
                if (op == '+') left.num += right.num;
                else left.num -= right.num;
            } else if (left.t == V_STR && right.t == V_STR && op == '+') {
                int l1 = strlen(left.str);
                strncpy(left.str + l1, right.str, MAXSTR - 1 - l1);
                left.str[MAXSTR - 1] = 0;
            }
        } else break;
    }
    return left;
}

static struct val eval_expr(struct env *e, const char **sp) {
    struct val left = eval_add(e, sp);
    const char *p = skip_ws(*sp);
    char op[3] = {0};
    if ((*p == '=' && *(p + 1) == '=') || (*p == '!' && *(p + 1) == '=') ||
        (*p == '<' && *(p + 1) == '=') || (*p == '>' && *(p + 1) == '=')) {
        op[0] = *p++; op[1] = *p++;
    } else if (*p == '<' || *p == '>') {
        op[0] = *p++;
    }
    if (op[0]) {
        *sp = p;
        struct val right = eval_add(e, sp);
        struct val res = { V_INT, 0, "" };
        if (left.t == V_INT && right.t == V_INT) {
            if (!strcmp(op, "==")) res.num = left.num == right.num;
            else if (!strcmp(op, "!=")) res.num = left.num != right.num;
            else if (!strcmp(op, "<")) res.num = left.num < right.num;
            else if (!strcmp(op, ">")) res.num = left.num > right.num;
            else if (!strcmp(op, "<=")) res.num = left.num <= right.num;
            else if (!strcmp(op, ">=")) res.num = left.num >= right.num;
        } else if (left.t == V_STR && right.t == V_STR) {
            int cmp = strcmp(left.str, right.str);
            if (!strcmp(op, "==")) res.num = cmp == 0;
            else if (!strcmp(op, "!=")) res.num = cmp != 0;
        }
        return res;
    }
    return left;
}

static void run_print(struct env *e, const char *p) {
    p = skip_ws(p);
    if (*p == '(') p++;
    int first = 1;
    while (*p && *p != ')') {
        if (!first) {
            p = skip_ws(p);
            if (*p == ',') { p++; emit(e, " "); }
        }
        p = skip_ws(p);
        if (*p == ')') break;
        struct val v = eval_expr(e, &p);
        if (v.t == V_INT) emit_num(e, v.num);
        else emit(e, v.str);
        first = 0;
        p = skip_ws(p);
    }
    emit(e, "\n");
}

static int line_indent(const char *s) {
    int n = 0;
    while (*s == ' ' || *s == '\t') {
        n += (*s == '\t') ? 4 : 1;
        s++;
    }
    return n;
}

int py_run(const char *src, char *out, int max) {
    struct env e;
    memset(&e, 0, sizeof e);
    e.out = out;
    e.max = max;
    e.pos = 0;
    out[0] = 0;

    static char lines[MAXL][128];
    static int indents[MAXL];
    int nlines = 0;

    const char *p = src;
    while (*p && nlines < MAXL) {
        while (*p == '\r' || *p == '\n') p++;
        if (!*p) break;
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        int len = (int)(eol - p);
        if (len >= 127) len = 127;
        memcpy(lines[nlines], p, len);
        lines[nlines][len] = 0;
        char *h = strchr(lines[nlines], '#');
        if (h) *h = 0;
        for (int i = strlen(lines[nlines]) - 1; i >= 0 && (lines[nlines][i] == ' ' || lines[nlines][i] == '\t'); i--)
            lines[nlines][i] = 0;
        indents[nlines] = line_indent(lines[nlines]);
        nlines++;
        p = eol;
    }

    int cur = 0, steps = 0;
    int loop_start[16], loop_end[16], loop_top = 0;
    char loop_var[16][32];
    i64 loop_cur[16], loop_max[16];

    while (cur < nlines && steps++ < 200000) {
        char *ln = lines[cur];
        const char *s = skip_ws(ln);
        if (!*s) { cur++; continue; }

        if (loop_top > 0 && cur == loop_end[loop_top - 1]) {
            loop_cur[loop_top - 1]++;
            if (loop_cur[loop_top - 1] < loop_max[loop_top - 1]) {
                set_var_int(&e, loop_var[loop_top - 1], loop_cur[loop_top - 1]);
                cur = loop_start[loop_top - 1];
                continue;
            } else {
                loop_top--;
            }
        }

        if (!strncmp(s, "print", 5) && (s[5] == '(' || s[5] == ' ')) {
            run_print(&e, s + 5);
            cur++;
            continue;
        }

        if (!strncmp(s, "for ", 4)) {
            const char *fp = skip_ws(s + 4);
            char vname[32]; int vi = 0;
            while (*fp && *fp != ' ' && vi < 31) vname[vi++] = *fp++;
            vname[vi] = 0;
            fp = skip_ws(fp);
            if (!strncmp(fp, "in ", 3)) {
                fp = skip_ws(fp + 3);
                if (!strncmp(fp, "range", 5)) {
                    fp = skip_ws(fp + 5);
                    if (*fp == '(') fp++;
                    struct val n1 = eval_expr(&e, &fp);
                    i64 st = 0, ed = n1.num;
                    fp = skip_ws(fp);
                    if (*fp == ',') {
                        fp++;
                        struct val n2 = eval_expr(&e, &fp);
                        st = n1.num;
                        ed = n2.num;
                    }
                    int base_ind = indents[cur];
                    int ecur = cur + 1;
                    while (ecur < nlines && (indents[ecur] > base_ind || !*skip_ws(lines[ecur]))) ecur++;

                    if (st < ed && loop_top < 16) {
                        strncpy(loop_var[loop_top], vname, 31);
                        loop_cur[loop_top] = st;
                        loop_max[loop_top] = ed;
                        loop_start[loop_top] = cur + 1;
                        loop_end[loop_top] = ecur;
                        set_var_int(&e, vname, st);
                        loop_top++;
                        cur++;
                    } else {
                        cur = ecur;
                    }
                    continue;
                }
            }
            cur++;
            continue;
        }

        if (!strncmp(s, "if ", 3)) {
            const char *ip = s + 3;
            struct val cval = eval_expr(&e, &ip);
            int base_ind = indents[cur];
            int ecur = cur + 1;
            while (ecur < nlines && (indents[ecur] > base_ind || !*skip_ws(lines[ecur]))) ecur++;
            if (cval.num) {
                cur++;
            } else {
                cur = ecur;
                if (cur < nlines) {
                    const char *es = skip_ws(lines[cur]);
                    if (!strncmp(es, "else:", 5)) cur++;
                }
            }
            continue;
        }

        if (!strncmp(s, "else:", 5)) {
            int base_ind = indents[cur];
            int ecur = cur + 1;
            while (ecur < nlines && (indents[ecur] > base_ind || !*skip_ws(lines[ecur]))) ecur++;
            cur = ecur;
            continue;
        }

        const char *eq = strchr(s, '=');
        if (eq && *(eq - 1) != '!' && *(eq - 1) != '<' && *(eq - 1) != '>' && *(eq + 1) != '=') {
            char vname[32]; int vi = 0;
            const char *np = s;
            while (np < eq && *np != ' ' && *np != '\t' && vi < 31) vname[vi++] = *np++;
            vname[vi] = 0;
            const char *valp = eq + 1;
            struct val res = eval_expr(&e, &valp);
            if (res.t == V_STR) set_var_str(&e, vname, res.str);
            else set_var_int(&e, vname, res.num);
            cur++;
            continue;
        }

        cur++;
    }

    if (!e.pos) emit(&e, "(Program exited normally with code 0)\n");
    return 0;
}
