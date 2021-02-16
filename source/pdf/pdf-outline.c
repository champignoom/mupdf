#include "mupdf/fitz.h"
#include "mupdf/pdf.h"

static fz_outline *
pdf_load_outline_imp(fz_context *ctx, pdf_document *doc, fz_outline *parent, pdf_obj *dict)
{
	fz_outline *node, **prev, *first = NULL, *prev_sib = NULL;
	pdf_obj *obj;
	pdf_obj *odict = dict;

	fz_var(dict);
	fz_var(first);

	fz_try(ctx)
	{
		prev = &first;
		while (dict && pdf_is_dict(ctx, dict))
		{
			if (pdf_mark_obj(ctx, dict))
				break;
			node = fz_new_outline(ctx);
			node->parent = parent;
			node->prev = prev_sib;
			prev_sib = node;
			*prev = node;
			prev = &node->next;

			obj = pdf_dict_get(ctx, dict, PDF_NAME(Title));
			if (obj)
				node->title = Memento_label(fz_strdup(ctx, pdf_to_text_string(ctx, obj)), "outline_title");

			if ((obj = pdf_dict_get(ctx, dict, PDF_NAME(Dest))) != NULL)
				node->uri = Memento_label(pdf_parse_link_dest(ctx, doc, obj), "outline_uri");
			else if ((obj = pdf_dict_get(ctx, dict, PDF_NAME(A))) != NULL)
				node->uri = Memento_label(pdf_parse_link_action(ctx, doc, obj, -1), "outline_uri");
			else
				node->uri = NULL;

			if (node->uri && !fz_is_external_link(ctx, node->uri))
				node->page = pdf_resolve_link(ctx, doc, node->uri, &node->x, &node->y);
			else
				node->page = -1;

			obj = pdf_dict_get(ctx, dict, PDF_NAME(First));
			if (obj)
			{
				node->down = pdf_load_outline_imp(ctx, doc, node, obj);

				obj = pdf_dict_get(ctx, dict, PDF_NAME(Count));
				if (pdf_to_int(ctx, obj) > 0)
					node->is_open = 1;
			}

			dict = pdf_dict_get(ctx, dict, PDF_NAME(Next));
		}
	}
	fz_always(ctx)
	{
		for (dict = odict; dict && pdf_obj_marked(ctx, dict); dict = pdf_dict_get(ctx, dict, PDF_NAME(Next)))
			pdf_unmark_obj(ctx, dict);
	}
	fz_catch(ctx)
	{
		fz_drop_outline(ctx, first);
		fz_rethrow(ctx);
	}

	return first;
}

fz_outline *
pdf_load_outline(fz_context *ctx, pdf_document *doc)
{
	pdf_obj *root, *obj, *first;
	fz_outline *outline = NULL;

	root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
	obj = pdf_dict_get(ctx, root, PDF_NAME(Outlines));
	first = pdf_dict_get(ctx, obj, PDF_NAME(First));
	if (first)
	{
		/* cache page tree for fast link destination lookups */
		pdf_load_page_tree(ctx, doc);
		fz_try(ctx)
			outline = pdf_load_outline_imp(ctx, doc, NULL, first);
		fz_always(ctx)
			pdf_drop_page_tree(ctx, doc);
		fz_catch(ctx)
			fz_rethrow(ctx);
	}

	return outline;
}

static void
pdf_clear_outline_imp(fz_context *ctx, pdf_document *doc, pdf_obj *first)
{
	if (first && !pdf_is_indirect(ctx, first))
		fz_throw(ctx, FZ_ERROR_GENERIC, "/First is not indirect");

	for (pdf_obj *dict=first; dict && pdf_is_dict(ctx, dict);)
	{
		pdf_obj *down, *next;

		down = pdf_dict_get(ctx, dict, PDF_NAME(First));
		if (down && !pdf_is_indirect(ctx, down))
			fz_throw(ctx, FZ_ERROR_GENERIC, "/Down is not indirect");

		next = pdf_dict_get(ctx, dict, PDF_NAME(Next));
		if (next && !pdf_is_indirect(ctx, next))
			fz_throw(ctx, FZ_ERROR_GENERIC, "/Next is not indirect");

		pdf_delete_object(ctx, doc, pdf_to_num(ctx, dict));

		if (down)
		{
			pdf_clear_outline_imp(ctx, doc, down);
		}

		dict = next;
	}
}

static void
pdf_clear_outline(fz_context *ctx, pdf_document *doc)
{
	pdf_obj *root, *outlines, *first;
	root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
	if (!root)
		fz_throw(ctx, FZ_ERROR_GENERIC, "/Root does not exist");
	if (!pdf_is_indirect(ctx, root))
		fz_throw(ctx, FZ_ERROR_GENERIC, "/Root is not indirect");

	// pdf_dict_del(ctx, root, PDF_NAME(Outlines)); return;  // FIXME

	outlines = pdf_dict_get(ctx, root, PDF_NAME(Outlines));
	if (outlines && !pdf_is_indirect(ctx, outlines))
		fz_throw(ctx, FZ_ERROR_GENERIC, "/Outlines is not indirect");

	first = pdf_dict_get(ctx, outlines, PDF_NAME(First));
	if (first && !pdf_is_indirect(ctx, first))
		fz_throw(ctx, FZ_ERROR_GENERIC, "/First is not indirect");

	if (first)
	{
		/* cache page tree for fast link destination lookups */
		pdf_load_page_tree(ctx, doc);
		fz_try(ctx) {
			pdf_clear_outline_imp(ctx, doc, first);
			pdf_delete_object(ctx, doc, pdf_to_num(ctx, outlines));
			pdf_dict_del(ctx, root, PDF_NAME(Outlines));
		}
		fz_always(ctx)
			pdf_drop_page_tree(ctx, doc);
		fz_catch(ctx)
			fz_rethrow(ctx);
	}
}

static void
pdf_write_outline_imp(fz_context *ctx, pdf_document *doc, fz_outline *first, pdf_obj *objIndParent, int parentIsOpen)
{
	pdf_obj *objIndFirst = NULL, *objIndLast = NULL;
	int cnt = 0;

	assert(objIndParent);
	assert(pdf_is_indirect(ctx, objIndParent));

	if (!first)
	{
		// do not put First, Last, Count into objIndParent
		return;
	}

	for (fz_outline *outline=first; outline; outline = outline->next)
	{
		pdf_obj *objIndThis, *arr, *page;

		objIndThis = pdf_add_new_dict(ctx, doc, 6);  // Title, Dest, Parent, First, Last, Count
		assert(objIndThis);
		assert(pdf_is_indirect(ctx, objIndThis));

		// /Title
		pdf_dict_put_text_string(ctx, objIndThis, PDF_NAME(Title), outline->title);

		// /Dest
		arr = pdf_new_array(ctx, doc, 5);  // #page, /XYZ, x, y, z
		pdf_dict_put(ctx, objIndThis, PDF_NAME(Dest), arr);
		//pdf_array_push_int(ctx._ctx, arr, 57);
		page = pdf_lookup_page_obj(ctx, doc, outline->page);
		if (!page)
			fz_throw(ctx, FZ_ERROR_GENERIC, "page %d does not exist", outline->page);
		assert(pdf_is_indirect(ctx, page));
		pdf_array_push(ctx, arr, page);
		pdf_array_push(ctx, arr, PDF_NAME(XYZ));
		{
			// adjusted from mupdf/source/pdf/pdf-link.c:109, pdf_parse_link_dest()
			int x, y, h;
			fz_rect mediabox;
			fz_matrix pagectm;

			pdf_page_obj_transform(ctx, page, &mediabox, &pagectm);
			mediabox = fz_transform_rect(mediabox, pagectm);
			h = mediabox.y1 - mediabox.y0;

			x = outline->x;
			y = outline->y!=0 ? h - outline->y : 0;

			pdf_array_push_int(ctx, arr, x);
			pdf_array_push_int(ctx, arr, y);
		}
		pdf_array_push(ctx, arr, 0);

		// Parent
		pdf_dict_put(ctx, objIndThis, PDF_NAME(Parent), objIndParent);

		if (!objIndFirst) {
			objIndFirst = objIndThis;
		}

		if (objIndLast) {
			pdf_dict_put(ctx, objIndLast, PDF_NAME(Next), objIndThis);
			pdf_dict_put(ctx, objIndThis, PDF_NAME(Prev), objIndLast);
		}
		objIndLast = objIndThis;

		pdf_write_outline_imp(ctx, doc, outline->down, objIndThis, outline->is_open);

		++cnt;
	}

	pdf_dict_put(ctx, objIndParent, PDF_NAME(First), objIndFirst);
	pdf_dict_put(ctx, objIndParent, PDF_NAME(Last), objIndLast);
	pdf_dict_put_int(ctx, objIndParent, PDF_NAME(Count), (parentIsOpen ?cnt :-cnt));
}

static void
pdf_write_outline(fz_context *ctx, pdf_document *doc, fz_outline *outline)
{
	pdf_obj *objIndRoot, *objIndOutline;

	if (!outline)
	{
		return;
	}

	objIndRoot = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
	objIndOutline = pdf_dict_get(ctx, objIndRoot, PDF_NAME(Outlines));
	if (objIndOutline)
	{
		fz_throw(ctx, FZ_ERROR_GENERIC, "outline already exists");
	}
	objIndOutline = pdf_add_new_dict(ctx, doc, 3);  // First, Last, Count
	pdf_write_outline_imp(ctx, doc, outline, objIndOutline, 1);
	pdf_dict_put(ctx, objIndRoot, PDF_NAME(Outlines), objIndOutline);
}

static const char *
check_outline_imp(fz_outline *first, fz_outline *parent)
{
	fz_outline *prev = NULL;

	if (first->prev)
	{
		return "first child's prev is not null";
	}

	for (fz_outline *outline=first; outline; prev=outline, outline=outline->next)
	{
		if (outline->parent != parent)
		{
			return "parent does not match";
		}
		if (outline->prev != prev)
		{
			return "prev does not match";
		}
		if (outline->down)
		{
			const char *msg = check_outline_imp(outline->down, outline);
			if (msg)
			{
				return msg;
			}
		}
	}
	return NULL;
}

static const char *
check_outline(fz_outline *outline)
{
	return check_outline_imp(outline, NULL);
}

void
pdf_rewrite_outline(fz_context *ctx, pdf_document *doc, fz_outline *outline)
{
	const char *msg = check_outline(outline);
	if (msg)
	{
		fz_throw(ctx, FZ_ERROR_GENERIC, "invalid outline: %s", msg);
	}
	pdf_clear_outline(ctx, doc);
	pdf_write_outline(ctx, doc, outline);
}
