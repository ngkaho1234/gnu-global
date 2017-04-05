/*
 * Copyright (c) 2017 Kaho Ng <ngkaho1234@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/types.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <malloc.h>
#ifdef STDC_HEADERS
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <clang-c/Index.h>

#include "parser.h"

/*
 * Structure to hold arguments passing to visit_children()
 */
struct visit_args {
	/* FILE handle to source file */
	FILE				*srcfilefp;

	/* Parser parameter from global */
	const struct parser_param	*param;
};

struct strbuf_s {
	char	*sbuf;
	int	sbufsize;
};
#define STRBUF_INITIALSIZE 80
#define STRBUF_EXPANDSIZE 80

/*
 * Expand buffer so that afford to the length data at least
 */
static int			/* positive for success, -1 for failure */
strbuf_expandbuf(
	struct strbuf_s	*sb,	/* strbuf_s structure */
	int		nslen)	/* new string length */
{
	char	*nsbuf;
	int	nbufsize = nslen + 1;	/* To hold \0 */

	if (sb->sbufsize >= nbufsize)
		return sb->sbufsize;

	nsbuf = (char *)realloc(sb->sbuf, nbufsize);
	if (!nsbuf)
		return -1;

	sb->sbufsize = nbufsize;
	sb->sbuf = nsbuf;

	return sb->sbufsize;
}

/*
 * Open string buffer
 */
static struct strbuf_s *	/* strbuf_s structure */
strbuf_open(int init)		/* initial buffer size */
{
	struct strbuf_s	*sb;
	char		*sbuf;
	int		sbufsize;

	sbufsize = (init > 0) ? init : STRBUF_INITIALSIZE;
	sb = (struct strbuf_s *)calloc(sizeof(struct strbuf_s), 1);
	if (!sb)
		return NULL;
	sbuf = (char *)malloc(sbufsize);
	if (!sbuf) {
		free(sb);
		return NULL;
	}

	sb->sbufsize = sbufsize;
	sb->sbuf = sbuf;

	return sb;
}

/*
 * Close string buffer
 */
static void
strbuf_close(struct strbuf_s *sb)	/* strbuf_s structure */
{
	free(sb->sbuf);
	free(sb);
}

/*
 * Return the content of string buffer
 */
static char *				/* string */
strbuf_value(struct strbuf_s *sb)	/* strbuf_s structure */
{
	int	endoffs;

	endoffs = sb->sbufsize - 1;
	sb->sbuf[endoffs] = 0;
	return sb->sbuf;
}

/*
 * Put string
 */
static int			/* 0 for success, -1 for failure */
strbuf_puts(
	struct strbuf_s	*sb,	/* strbuf_s structure */
	const char	*s)	/* string */
{
	char	*p;

	/*
	 * Make sure that we got enought space to hold the content
	 */
	if (strbuf_expandbuf(sb, strlen(s)) < 0)
		return -1;

	p = sb->sbuf;
	while (*s) {
		*p++ = *s++;
	}
	*p = 0;

	return 0;
}

/*
 * Do sprintf into string buffer
 */
static int			/* non-negative for success, -1 for failure */
strbuf_sprintf(
	struct strbuf_s	*sb,	/* strbuf_s structure */
	const char	*fmt,	/* format string */
	...)			/* ARGS */
{
	va_list	ap;
	int	ret;
	int	slen;
	int	sbufsize;

	va_start(ap, fmt);
	ret = slen = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (ret < 0)
		goto out;

	ret = sbufsize = strbuf_expandbuf(sb, slen);
	if (ret < 0)
		goto out;

	va_start(ap, fmt);
	ret = vsnprintf(sb->sbuf, sbufsize, fmt, ap);
	va_end(ap);

out:
	return ret;
}

/*
 * Free the line buffer returned by getline()
 */
static void
free_line_buf(char *linebuf)	/* Line buffer */
{
	free(linebuf);
}

/*
 * Read #line-th line from @srcfilefp
 *
 * Return number of bytes read, address to line buffer
 * and size of line buffer.
 */
static ssize_t
read_line_no(
	FILE		*srcfilefp,	/* FILE handle */
	unsigned int	line,		/* line number to be read */
	char		**linebufp,	/* line buffer returned */
	size_t		*linebuflenp)	/* line buffer size returned */
{
	unsigned int	i;
	ssize_t		ret = 0;

	assert(line);

	rewind(srcfilefp);
	for (i = 0; i < line; ++i) {
		/*
		 * getline() may reallocate a larger buffer
		 * to hold more content.
		 */
		ret = getline(linebufp, linebuflenp, srcfilefp);
		if (ret <= 0)
			break;
	}

	return ret;
}

/*
 * Check if a cursor points to a definition
 */
static int
is_definition(CXCursor cursor)	/* Cursor */
{
	enum CXCursorKind	cursorkind;

	cursorkind = clang_getCursorKind(cursor);
	return (clang_isCursorDefinition(cursor) &&
	    cursorkind != CXCursor_CXXAccessSpecifier &&
	    cursorkind != CXCursor_TemplateTypeParameter &&
	    cursorkind != CXCursor_UnexposedDecl)
		||
	    (clang_isDeclaration(cursorkind) &&
	     cursorkind == CXCursor_VarDecl);
}

/*
 * Check if a cursor points to a reference
 */
static int
is_reference(CXCursor cursor)	/* Cursor */
{
	enum CXCursorKind	cursorkind;

	cursorkind = clang_getCursorKind(cursor);
	return cursorkind == CXCursor_DeclRefExpr;
}

/*
 * Check if a cursor should be tagged
 */
static int
should_tag(CXCursor cursor)	/* Cursor */
{
	enum CXLinkageKind	linkagekind;
	CXCursor		refcursor;

	if (!is_definition(cursor) && !is_reference(cursor))
		return 0;

	refcursor = clang_getCursorReferenced(cursor);
	linkagekind = clang_getCursorLinkage(refcursor);
	return linkagekind != CXLinkage_Invalid &&
	    linkagekind != CXLinkage_NoLinkage;
}

/*
 * Prepend semantic parent to the content of a string buffer
 */
static int				/* 0 for success, -1 for failure */
prepend_semantic_parent(
	struct strbuf_s	*sb,		/* Cursor */
	const char	*spspelling)	/* Spelling of semantic parent */
{
	char	*tsbuf;
	int	tslen;
	int	ret = 0;

	tslen = strlen(strbuf_value(sb));
	tsbuf = malloc(tslen + 1);
	if (!tsbuf)
		return -1;
	tsbuf[tslen] = 0;

	memcpy(tsbuf, strbuf_value(sb), tslen);
	if (strbuf_sprintf(sb, "%s::%s", spspelling, tsbuf) < 0)
		ret = -1;

	free(tsbuf);
	return ret;
}

/*
 * Check if a cursor points to a named scope
 */
static int
is_named_scope(CXCursor cursor)	/* Cursor */
{
	enum CXCursorKind	cursorkind;

	cursorkind = clang_getCursorKind(cursor);
	return cursorkind == CXCursor_Namespace ||
	    cursorkind == CXCursor_StructDecl ||
	    cursorkind == CXCursor_UnionDecl ||
	    cursorkind == CXCursor_EnumDecl ||
	    cursorkind == CXCursor_ClassDecl ||
	    cursorkind == CXCursor_ClassTemplate ||
	    cursorkind == CXCursor_ClassTemplatePartialSpecialization;
}

/*
 * Prepend spelling of each semantic parents of a cursor
 * to a string buffer
 */
static int			/* 0 for success, -1 for failure */
fix_tag_semantic_parent(
	CXCursor	cursor,
	struct strbuf_s	*sb)
{
	int		ret = 0;
	CXCursor	spcursor;

	spcursor = clang_getCursorSemanticParent(cursor);
	while (!clang_Cursor_isNull(spcursor) &&
	       is_named_scope(spcursor)) {
		CXCursor	nspcursor;
		CXString	spspellingstr;
		const char	*spspelling;

		spspellingstr = clang_getCursorSpelling(spcursor);
		spspelling = clang_getCString(spspellingstr);

		if (!spspelling)
			spspelling = "";

		ret = prepend_semantic_parent(sb, spspelling);
		clang_disposeString(spspellingstr);
		if (ret < 0)
			break;

		/*
		 * Find into the semantic parent even further if viable
		 */
		nspcursor = clang_getCursorSemanticParent(cursor);
		if (clang_equalCursors(spcursor, nspcursor))
			break;
		spcursor = nspcursor;
	}

	return ret;
}

/*
 * AST walker routine
 *
 * If we produce a definition or reference tag, we pass
 * them to Global.
 *
 * Return CXChildVisit_Break if we fail somewhere,
 * otherwise CXChildVisit_Recurse.
 */
enum CXChildVisitResult
visit_children(
	CXCursor	cursor,	/* Current cursor */
	CXCursor	parent,	/* Parent cursor */
	CXClientData	data)	/* Arguments */
{
	CXSourceLocation		loc;
	CXFile				file;
	unsigned int			line;
	unsigned int			column;
	unsigned int			offs;
	CXString			pathcxstr;
	CXString			usrcxstr;
	CXString			spellcxstr;
	const char			*pathstr;
	const char			*spellstr;
	const struct visit_args		*argsp;
	const struct parser_param	*param;
	enum CXChildVisitResult		ret;

	argsp = (const struct visit_args *)data;
	param = argsp->param;
	ret = CXChildVisit_Recurse;

	/*
	 * Get the location @cursor points to
	 */
	loc = clang_getCursorLocation(cursor);
	clang_getSpellingLocation(loc, &file, &line, &column, &offs);

	pathcxstr = clang_getFileName(file);
	spellcxstr = clang_getCursorSpelling(cursor);

	pathstr = clang_getCString(pathcxstr);
	spellstr = clang_getCString(spellcxstr);

	/*
	 * It is possible that @pathstr and @spellstr
	 * would be set to NULL.
	 */
	if (!pathstr)
		pathstr = "";
	if (!spellstr)
		spellstr = "";

	if (strcmp(param->file, pathstr)) {
		goto out;
	}
	
	if (should_tag(cursor)) {
		ssize_t		nread;
		ssize_t		linelen;
		size_t		linebuflen = 0;
		char		*linebuf = NULL;
		struct strbuf_s	*sb = NULL;

		/*
		 * Fetch the required line from the source file
		 */
		nread = read_line_no(argsp->srcfilefp,
				     line,
				     &linebuf,
				     &linebuflen);
		if (nread <= 0) {
			ret = CXChildVisit_Break;
			goto cleanup;
		}
		linelen = nread;

		sb = strbuf_open(0);
		if (!sb) {
			ret = CXChildVisit_Break;
			goto cleanup;
		}

		/*
		 * Remove trailing newline character
		 */
		while (nread--) {
			if (linebuf[nread] != '\r' &&
			    linebuf[nread] != '\n') {
				break;
			}
			linebuf[nread] = 0;
		}


		/*
		 * Pass the tag information we gathered to Global
		 */
		if (is_definition(cursor)) {
			if (strbuf_puts(sb, spellstr) < 0) {
				ret = CXChildVisit_Break;
				goto cleanup;
			}
			if (fix_tag_semantic_parent(cursor, sb) < 0) {
				ret = CXChildVisit_Break;
				goto cleanup;
			}

			param->put(PARSER_DEF,
				   strbuf_value(sb),
				   line,
				   pathstr,
				   linebuf,
				   param->arg);
		} else if (is_reference(cursor)) {
			if (strbuf_puts(sb, spellstr) < 0) {
				ret = CXChildVisit_Break;
				goto cleanup;
			}

			param->put(PARSER_REF_SYM,
				   strbuf_value(sb),
				   line,
				   pathstr,
				   linebuf,
				   param->arg);
		}

cleanup:
		if (sb)
			strbuf_close(sb);
		free_line_buf(linebuf);
	}

out:
	clang_disposeString(pathcxstr);
	clang_disposeString(spellcxstr);
	return ret;
}

/*
 * Main parser plugin routine
 *
 * This routine passes definition tags and reference tags
 * to Global at once when cursors representing definitions
 * or references are discovered.
 */
void
parser(const struct parser_param *param)	/* Parser parameters */
{
	CXIndex			cxindex;
	CXTranslationUnit	tu;
	struct visit_args	visitargs;

	assert(param->size >= sizeof(*param));

	cxindex = clang_createIndex(0, 0);
	tu = clang_createTranslationUnitFromSourceFile(cxindex,
						       param->file,
						       0,
						       NULL,
						       0,
						       NULL);
	if (!tu)
		return;

	/*
	 * If we cannot open the source file, there is nothing
	 * we can do. Just bail out in this case.
	 */
	visitargs.srcfilefp = fopen(param->file, "r");
	if (!visitargs.srcfilefp)
		goto out;
	visitargs.param = param;

	/*
	 * Now we start to walk the AST tree return by libclang,
	 * and pass the definition and reference tags to Global
	 * once we found some cursors representing definitions or
	 * references.
	 */
	clang_visitChildren(clang_getTranslationUnitCursor(tu),
			    visit_children,
			    &visitargs);

	fclose(visitargs.srcfilefp);
out:
	clang_disposeTranslationUnit(tu);
	clang_disposeIndex(cxindex);
}
