/*
 * jgmenu-ob.c
 *
 * Parses openbox menu and outputs a jgmenu-flavoured CSV file
 */

#include <stdio.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"
#include "sbuf.h"
#include "list.h"

static char root_menu_default[] = "root-menu";
static char *root_menu = root_menu_default;

struct tag {
	char *label;
	char *id;
	struct tag *parent;
	struct list_head items;
	struct list_head list;
};

struct item {
	char *label;
	char *cmd;
	int pipe;
	int checkout;
	int isseparator;
	struct list_head list;
};

static struct list_head tags;
static struct tag *curtag;
static struct item *curitem;

static void print_it(struct tag *tag)
{
	struct item *item;
	struct sbuf label_escaped;

	if (list_empty(&tag->items))
		return;
	printf("%s,^tag(%s)\n", tag->label, tag->id);
	if (tag->parent)
		printf("Back,^back()\n");
	list_for_each_entry(item, &tag->items, list) {
		sbuf_init(&label_escaped);
		sbuf_cpy(&label_escaped, item->label);
		sbuf_replace(&label_escaped, "&", "&amp;");
		if (item->pipe)
			printf("%s,^pipe(jgmenu_run ob --cmd='%s' --tag='%s')\n",
			       label_escaped.buf, item->cmd, item->label);
		else if (item->checkout)
			printf("%s,^checkout(%s)\n", label_escaped.buf, item->cmd);
		else if (item->isseparator)
			printf("^sep(%s)\n", label_escaped.buf);
		else
			printf("%s,%s\n", label_escaped.buf, item->cmd);
		xfree(label_escaped.buf);
	}
	printf("\n");
}

static void print_menu(void)
{
	struct tag *tag;

	list_for_each_entry(tag, &tags, list)
		if (tag->id && !strcmp(tag->id, root_menu))
			print_it(tag);

	list_for_each_entry(tag, &tags, list)
		if (tag->id && strcmp(tag->id, root_menu))
			print_it(tag);
}

static char *get_tag_label(const char *id)
{
	struct tag *tag;

	if (!id)
		return NULL;
	list_for_each_entry(tag, &tags, list)
		if (tag->id && !strcmp(tag->id, id))
			return tag->label;
	return NULL;
}

static struct tag *get_parent_tag(xmlNode *n)
{
	struct tag *tag;
	char *id = NULL;

	if (!n || !n->parent)
		goto out;

	/* ob pipe-menus don't wrap first level in <menu></menu> */
	if (!strcmp((char *)n->parent->name, "openbox_pipe_menu"))
		id = root_menu;
	else
		id = (char *)xmlGetProp(n->parent, (const xmlChar *)"id");
	if (!id)
		goto out;
	list_for_each_entry(tag, &tags, list)
		if (tag->id && !strcmp(tag->id, id))
			return tag;
out:
	return NULL;
}

static void new_tag(xmlNode *n);

static void new_item(xmlNode *n, int isseparator)
{
	struct item *item;
	char *label = (char *)xmlGetProp(n, (const xmlChar *)"label");

	if (!curtag)
		new_tag(NULL);

	item = xmalloc(sizeof(struct item));
	item->label = NULL;
	if (label)
		item->label = label;
	item->cmd = NULL;
	item->pipe = 0;
	item->checkout = 0;
	if (isseparator)
		item->isseparator = 1;
	else
		item->isseparator = 0;
	curitem = item;
}

static void new_tag(xmlNode *n)
{
	struct tag *t = xcalloc(1, sizeof(struct tag));
	struct tag *parent = get_parent_tag(n);
	char *label = (char *)xmlGetProp(n, (const xmlChar *)"label");
	char *id = (char *)xmlGetProp(n, (const xmlChar *)"id");

	/*
	 * The pipe-menu "root" has no <menu> element and therefore no
	 * LABEL or ID.
	 */
	if (id)
		t->id = id;
	else
		t->id = root_menu;
	t->label = label;
	t->parent = parent;
	INIT_LIST_HEAD(&t->items);
	list_add_tail(&t->list, &tags);
	curtag = t;

	if (parent && strcmp(id, root_menu) != 0) {
		new_item(n, 0);
		curitem->label = label;
		curitem->cmd = id;
		curitem->checkout = 1;
		list_add_tail(&curitem->list, &curtag->parent->items);
	}
}

static void revert_to_parent(void)
{
	if (curtag && curtag->parent)
		curtag = curtag->parent;
}

static int node_filter(const xmlChar *name)
{
	return strcasecmp((char *)name, "menu");
}

static void get_full_node_name(struct sbuf *node_name, xmlNode *node)
{
	int incl;

	if (!strcmp((char *)node->name, "text")) {
		node = node->parent;
		if (!node || !node->name) {
			fprintf(stderr, "warning: node is root\n");
			return;
		}
	}

	incl = node_filter(node->name);
	for (;;) {
		if (incl)
			sbuf_prepend(node_name, (char *)node->name);
		node = node->parent;
		if (!node || !node->name)
			return;
		incl = node_filter(node->name);
		if (incl)
			sbuf_prepend(node_name, ".");
	}
}

static void get_special_action(xmlNode *node, char **cmd)
{
	char *action;

	action = (char *)xmlGetProp(node, (const xmlChar *)"name");
	if (!action)
		return;
	if (!strcasecmp(action, "Execute"))
		return;
	if (!strcasecmp(action, "reconfigure"))
		*cmd = strdup("openbox --reconfigure");
	else if (!strcasecmp(action, "restart"))
		*cmd = strdup("openbox --restart");
}

static void process_node(xmlNode *node)
{
	struct sbuf buf;
	struct sbuf node_name;

	sbuf_init(&buf);
	sbuf_init(&node_name);
	get_full_node_name(&node_name, node);
	if (!node_name.len)
		return;

	if (strstr(node_name.buf, "item.action.command") && node->content)
		/* <command></command> */
		curitem->cmd = strstrip((char *)node->content);
	else if (strstr(node_name.buf, "item.action"))
		/* Catch <action name="Reconfigure"> and <action name="Restart"> */
		get_special_action(node, &curitem->cmd);
	xfree(buf.buf);
	xfree(node_name.buf);
}

/*
 * <menu> elements can be three things:
 *
 *  - "normal" menu (gets tag). Has ID, LABEL and CONTENT
 *  - "pipe" menu. Has EXECUTE and LABEL
 *  - Link to a menu defined else where. Has ID only.
 */
static int menu_start(xmlNode *n)
{
	int ret = 0;
	char *label;
	char *execute;
	char *id = NULL;

	label = (char *)xmlGetProp(n, (const xmlChar *)"label");
	execute = (char *)xmlGetProp(n, (const xmlChar *)"execute");
	id = (char *)xmlGetProp(n, (const xmlChar *)"id");

	if (label && !execute) {
		/* new ^tag() */
		new_tag(n);
		ret = 1;
	} else if (execute) {
		/* pipe-menu */
		new_item(n, 0);
		curitem->pipe = 1;
		curitem->cmd = execute;
		list_add_tail(&curitem->list, &curtag->items);
	} else if (id) {
		/* checkout a menu defined elsewhere */
		new_item(n, 0);
		curitem->checkout = 1;
		curitem->cmd = id;
		curitem->label = get_tag_label(id);
		list_add_tail(&curitem->list, &curtag->items);
	}

	return ret;
}

static void xml_tree_walk(xmlNode *node)
{
	xmlNode *n;
	int ret;

	for (n = node; n && n->name; n = n->next) {
		if (!strcasecmp((char *)n->name, "menu")) {
			ret = menu_start(n);
			xml_tree_walk(n->children);
			if (ret)
				revert_to_parent();
			continue;
		}
		if (!strcasecmp((char *)n->name, "item")) {
			new_item(n, 0);
			list_add_tail(&curitem->list, &curtag->items);
			xml_tree_walk(n->children);
			continue;
		}
		if (!strcasecmp((char *)n->name, "separator")) {
			new_item(n, 1);
			list_add_tail(&curitem->list, &curtag->items);
			xml_tree_walk(n->children);
			continue;
		}
		if (!strcasecmp((char *)n->name, "Comment"))
			continue;

		process_node(n);
		xml_tree_walk(n->children);
	}
}

static void parse_xml(struct sbuf *xmlbuf)
{
	xmlDoc *d;

	d = xmlParseMemory(xmlbuf->buf, strlen(xmlbuf->buf));
	if (!d)
		exit(1);
	xml_tree_walk(xmlDocGetRootElement(d));
	print_menu();
	xmlFreeDoc(d);
	xmlCleanupParser();
}

static void cleanup(void)
{
	struct tag *tag, *tag_tmp;
	struct item *item, *i_tmp;

	list_for_each_entry(tag, &tags, list) {
		list_for_each_entry_safe(item, i_tmp, &tag->items, list) {
			list_del(&item->list);
			xfree(item);
		}
	}
	list_for_each_entry_safe(tag, tag_tmp, &tags, list) {
		list_del(&tag->list);
		xfree(tag);
	}
}

void handle_argument_clash(void)
{
	die("both --cmd=<cmd> and <file> provided");
}

int main(int argc, char **argv)
{
	int i;
	struct sbuf default_file;
	FILE *fp = NULL;
	struct sbuf xmlbuf;
	char buf[BUFSIZ], *p;

	atexit(cleanup);
	LIBXML_TEST_VERSION

	i = 1;
	while (i < argc) {
		if (argv[i][0] != '-') {
			if (argc > i + 1)
				die("<file> must be the last argument");
			if (fp)
				handle_argument_clash();
			fp = fopen(argv[i], "r");
			if (!fp)
				die("ob: cannot open file '%s'", argv[i]);
		} else if (!strncmp(argv[i], "--tag=", 6)) {
			root_menu = argv[i] + 6;
		} else if (!strncmp(argv[i], "--cmd=", 6)) {
			fp = popen(argv[i] + 6, "r");
			if (!fp)
				die("ob: cannot run command '%s'", argv[i] + 6);
		}
		i++;
	}
	if (!fp) {
		sbuf_init(&default_file);
		sbuf_cpy(&default_file, getenv("HOME"));
		sbuf_addstr(&default_file, "/.config/openbox/menu.xml");
		fp = fopen(default_file.buf, "r");
	}
	if (!fp)
		die("ob: cannot open openbox menu file");
	INIT_LIST_HEAD(&tags);
	sbuf_init(&xmlbuf);
	for (i = 0; fgets(buf, sizeof(buf), fp); i++) {
		buf[BUFSIZ - 1] = '\0';
		p = strrchr(buf, '\n');
		if (p)
			*p = '\0';
		sbuf_addstr(&xmlbuf, buf);
	}
	parse_xml(&xmlbuf);
	xfree(xmlbuf.buf);

	return 0;
}
