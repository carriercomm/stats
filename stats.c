#include <ccan/err/err.h>
#include <ccan/opt/opt.h>
#include <ccan/rbuf/rbuf.h>
#include <ccan/htable/htable_type.h>
#include <ccan/hash/hash.h>
#include <ccan/list/list.h>
#include <ccan/str/str.h>
#include <unistd.h>
#include <errno.h>

enum pattern_type {
	LITERAL,
	INTEGER,
	FLOAT,
	/* These are parsing states. */
	PRESPACES,
	TERM
};

union val {
	long long ival;
	double dval;
};

struct pattern_part {
	enum pattern_type type;
	size_t off, len;
};

struct pattern {
	const char *text;
	size_t num_parts;
	struct pattern_part part[ /* num_parts */ ];
};

struct line {
	struct list_node list;
	struct pattern *pattern;
	size_t count;

	/* An array of count arrays of pattern->num_parts elements. */
	union val *vals;
};

static const struct pattern *line_key(const struct line *line)
{
	return line->pattern;
}

static size_t pattern_hash(const struct pattern *p)
{
	size_t i;
	size_t h = p->num_parts; 

	for (i = 0; i < p->num_parts; i++) {
		const struct pattern_part *part = &p->part[i];

		if (part->type == LITERAL)
			h = hash(p->text + part->off, part->len, h);
	}
	return h;
}

static bool line_eq(const struct line *line, const struct pattern *p)
{
	const struct pattern *p2 = line->pattern;
	size_t i;

	if (p->num_parts != p2->num_parts)
		return false;
	for (i = 0; i < p->num_parts; i++) {
		const struct pattern_part *part1 = &p->part[i];
		const struct pattern_part *part2 = &p2->part[i];

		if (part1->type == LITERAL) {
			if (part2->type != LITERAL)
				return false;
			if (part1->len != part2->len)
				return false;
			if (strncmp(p->text + part1->off,
				    p2->text + part2->off,
				    part1->len))
				return false;
		} else if (part2->type == LITERAL)
			return false;
	}
	return true;
}

HTABLE_DEFINE_TYPE(struct line, line_key, pattern_hash, line_eq, linehash);

struct file {
	struct list_head lines;
	struct linehash patterns;
};

static inline size_t partsize(size_t num)
{
	return sizeof(struct pattern) + sizeof(struct pattern_part) * num;
}

static void add_part(struct pattern **p, union val **vals,
		     const struct pattern_part *part, const union val *v,
		     size_t *max_parts)
{
	if ((*p)->num_parts == *max_parts) {
		*max_parts *= 2;
		*p = realloc(*p, partsize(*max_parts));
		*vals = realloc(*vals, *max_parts * sizeof(*v));
	}
	(*vals)[(*p)->num_parts] = *v;
	(*p)->part[(*p)->num_parts++] = *part;
}

/* We want "finished in100 seconds to match "finished in  5 seconds". */
struct pattern *get_pattern(const char *line, union val **vals)
{
	enum pattern_type state = LITERAL;
	size_t len, i, max_parts = 3;
	struct pattern_part part;
	struct pattern *p;

	*vals = malloc(sizeof(union val) * max_parts);
	p = malloc(partsize(max_parts));
	p->text = line;
	p->num_parts = 0;

	for (i = len = 0; state != TERM; i++, len++) {
		enum pattern_type old_state = state;
		bool starts_num;
		union val v;

		starts_num = (line[i] == '-' && cisdigit(line[i+1]))
			|| cisdigit(line[i]);

		switch (state) {
		case LITERAL:
			if (starts_num) {
				state = INTEGER;
				break;
			} else if (cisspace(line[i])) {
				state = PRESPACES;
				break;
			}
			break;
		case PRESPACES:
			if (starts_num) {
				state = INTEGER;
				break;
			} else if (!cisspace(line[i])) {
				state = LITERAL;
			}
			break;
		case INTEGER:
			if (line[i] == '.') {
				if (cisdigit(line[i+1])) {
					/* Was float all along... */
					state = old_state = FLOAT;
				} else
					state = LITERAL;
				break;
			}
			/* fall thru */
		case FLOAT:
			if (cisspace(line[i])) {
				state = PRESPACES;
				break;
			} else if (!cisdigit(line[i])) {
				state = LITERAL;
				break;
			}
			break;
		case TERM:
			abort();
		}

		if (!line[i])
			state = TERM;

		if (state == old_state)
			continue;

		part.type = old_state;
		part.len = len;
		part.off = i - len;
		if (old_state == FLOAT) {
			char *end;
			v.dval = strtod(line + part.off, &end);
			if (end != line + i) {
				warnx("Could not parse float '%.*s'",
				      (int)len, line + i - len);
			} else {
				add_part(&p, vals, &part, &v, &max_parts);
			}
			len = 0;
		} else if (old_state == INTEGER) {
			char *end;
			v.ival = strtoll(line + part.off, &end, 10);
			if (end != line + i) {
				warnx("Could not parse integer '%.*s'",
				      (int)len, line + i - len);
			} else {
				add_part(&p, vals, &part, &v, &max_parts);
			}
			len = 0;
		} else if (old_state == LITERAL && len > 0) {
			/* Since we can go to PRESPACES and back, we can
			 * have successive literals.  Collapse them. */
			if (p->num_parts > 0
			    && p->part[p->num_parts-1].type == LITERAL) {
				p->part[p->num_parts-1].len += len;
				len = 0;
				continue;
			}
			add_part(&p, vals, &part, &v, &max_parts);
			len = 0;
		}
	}
	return p;
}

static void val_to_float(union val *val)
{
	val->dval = val->ival;
}

static void add_stats(struct line *line, struct pattern *p, union val *vals)
{
	size_t i;

	line->vals = realloc(line->vals, 
			     sizeof(union val) * (line->count+1) * p->num_parts);

	for (i = 0; i < p->num_parts; i++) {
		if (p->part[i].type == LITERAL)
			continue;
		if (p->part[i].type == FLOAT
		    && line->pattern->part[i].type == INTEGER) {
			size_t j;
			/* Convert all previous entries to float. */
			for (j = 0; j < line->count; j++)
				val_to_float(&line->vals[j * p->num_parts + i]);
			line->pattern->part[i].type = FLOAT;
		} else if (p->part[i].type == INTEGER
			   && line->pattern->part[i].type == FLOAT) {
			val_to_float(&vals[i]);
			p->part[i].type = FLOAT;
		}
		assert(p->part[i].type == line->pattern->part[i].type);
		memcpy(line->vals + line->count * p->num_parts, vals,
		       sizeof(*vals) * p->num_parts);
	}
	line->count++;
}

static void add_line(struct file *info, const char *str)
{
	struct line *line;
	struct pattern *p;
	union val *vals;

	p = get_pattern(str, &vals);

	line = linehash_get(&info->patterns, p);
	if (line) {
		add_stats(line, p, vals);
	} else {
		/* We need to keep a copy of this! */
		p->text = strdup(p->text); 
		line = malloc(sizeof(*line));
		line->pattern = p;
		line->count = 1;
		line->vals = vals;
		linehash_add(&info->patterns, line);
		list_add_tail(&info->lines, &line->list);
	}
}

static void print_literal_part(const struct pattern *p, size_t off)
{
	printf("%.*s", (int)p->part[off].len, p->text + p->part[off].off);
}

static const char *spacestart(const struct pattern *p, size_t off)
{
	if (cisspace(p->text[p->part[off].off]))
		return " ";
	else
		return "";
}

static void print_float(union val *vals, const struct pattern *p,
			size_t num, size_t num_parts, size_t off)
{
	size_t i;
	double min = vals[off].dval, max = vals[off].dval, tot = vals[off].dval;

	for (i = 1; i < num; i++) {
		if (vals[off + i * num_parts].dval < min)
			min = vals[off + i * num_parts].dval;
		else if (vals[off + i * num_parts].dval > max)
			max = vals[off + i * num_parts].dval;
		tot += vals[off + i * num_parts].dval;
	}

	if (min == max)
		print_literal_part(p, off);
	else
		printf("%s%lf-%lf(%lf)", spacestart(p, off),
		       min, max, tot / num);
}

static void print_int(union val *vals, const struct pattern *p,
		      size_t num, size_t num_parts, size_t off)
{
	size_t i;
	long long min = vals[off].ival, max = vals[off].ival, tot = vals[off].ival;

	for (i = 1; i < num; i++) {
		if (vals[off + i * num_parts].ival < min)
			min = vals[off + i * num_parts].ival;
		else if (vals[off + i * num_parts].ival > max)
			max = vals[off + i * num_parts].ival;
		tot += vals[off + i * num_parts].ival;
	}

	if (min == max)
		print_literal_part(p, off);
	else
		printf("%s%lli-%lli(%lli)", spacestart(p, off),
		       min, max, tot / num);
}

static void print_analysis(const struct file *info, bool trim_outliers)
{
	struct line *l;

	/* FIXME: trim_outliers! */
	list_for_each(&info->lines, l, list) {
		size_t i;

		for (i = 0; i < l->pattern->num_parts; i++) {
			switch (l->pattern->part[i].type) {
			case LITERAL:
				print_literal_part(l->pattern, i);
				break;
			case FLOAT:
				print_float(l->vals, l->pattern,
					    l->count, l->pattern->num_parts, i);
				break;
			case INTEGER:
				print_int(l->vals, l->pattern,
					  l->count, l->pattern->num_parts, i);
				break;
			default:
				abort();
			}

		}
		fputc('\n', stdout);
	}
}

int main(int argc, char *argv[])
{
	bool trim_outliers = false;

	opt_register_noarg("--trim-outliers", opt_set_bool, &trim_outliers,
			   "Remove max and min results from average");
	opt_register_noarg("-h|--help", opt_usage_and_exit,
			   "\nA program to print max-min(avg) stats in place"
			   "of numbers in a stream", "Print this message");
	opt_parse(&argc, argv, opt_log_stderr_exit);

	do {
		struct file info;
		struct rbuf in;
		char *str;

		list_head_init(&info.lines);
		linehash_init(&info.patterns);

		if (argv[1]) {
			if (!rbuf_open(&in, argv[1], NULL, 0))
				err(1, "Failed opening %s", argv[1]);
		} else
			rbuf_init(&in, STDIN_FILENO, NULL, 0);

		while ((str = rbuf_read_str(&in, '\n', realloc)) != NULL)
			add_line(&info, str);

		if (errno)
			err(1, "Reading %s", argv[1] ? argv[1] : "<stdin>");

		print_analysis(&info, trim_outliers);
	} while (argv[1] && (++argv)[1]);
	return 0;
}
