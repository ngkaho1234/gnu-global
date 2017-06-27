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
#include <leveldb/c.h>

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

/*
 * Buffer to store string
 */
struct strbuf_s {
	/* string buffer */
	char	*sbuf;

	/* allocated size of the buffer */
	int	sbufsize;

	/*
	 * string length
	 *
	 * Remark: @slen has to be smaller than @sbufsize
	 * 	   in order to store an extra \0 right after
	 * 	   the last byte of the string.
	 */
	int	slen;
};
#define STRBUF_INITIALSIZE 80
#define STRBUF_EXPANDSIZE 80

/*
 * Expand buffer so that afford to the length data at least
 *
 * Return positive buffer size for success, -1 or 0 for failure.
 */
static int
strbuf_expandbuf(
	struct strbuf_s	*sb,	/* strbuf_s structure */
	int		nslen)	/* new string length */
{
	char	*nsbuf;
	int	nbufsize = nslen + 1;	/* To hold \0 */

	/*
	 * No need to enlarge the buffer if it is already
	 * sufficiently large
	 */
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

	/*
	 * Certain size of space will be pre-allocated if
	 * @init is not specified
	 */
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
	sb->slen = 0;

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

	/*
	 * Set the first byte right after the last byte of the
	 * string to 0x0 to mark it the end of string
	 */
	endoffs = sb->slen;
	sb->sbuf[endoffs] = 0;
	return sb->sbuf;
}

/*
 * Put string into string buffer
 */
static int			/* 0 for success, -1 for failure */
strbuf_puts(
	struct strbuf_s	*sb,	/* strbuf_s structure */
	const char	*s)	/* string */
{
	char	*p;
	int	slen;

	slen = strlen(s);

	/*
	 * Make sure that we got enought space to hold the content
	 * including \0
	 */
	if (strbuf_expandbuf(sb, slen) <= 0)
		return -1;

	p = sb->sbuf;
	while (*s) {
		*p++ = *s++;
	}
	*p = 0;

	sb->slen = slen;
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

	/*
	 * Make sure that we got enought space to hold the content
	 * including \0
	 */
	ret = sbufsize = strbuf_expandbuf(sb, slen);
	if (ret <= 0)
		goto out;

	va_start(ap, fmt);
	ret = vsnprintf(sb->sbuf, sbufsize, fmt, ap);
	va_end(ap);

	sb->slen = slen;

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
 * Read file data from @srcfilefp
 *
 * Return number of bytes read if successful,
 * otherwise -1.
 */
static ssize_t
read_file_data(
	FILE		*srcfilefp,	/* FILE handle */
	off_t		startoffs,	/* Start offset to read from */
	size_t		len,		/* Size to be read */
	char		*buf)		/* buffer */
{
	ssize_t		ret = 0;

	/*
	 * Seek to the start offset of data to be read
	 */
	ret = fseeko(srcfilefp, startoffs, SEEK_SET);
	if (ret < 0)
		return -1;

	ret = fread(buf, 1, len, srcfilefp);
	if (ferror(srcfilefp)) {
		clearerr(srcfilefp);
		ret = -1;
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
	if (clang_isCursorDefinition(cursor)) {
		if (cursorkind != CXCursor_CXXAccessSpecifier &&
		    cursorkind != CXCursor_TemplateTypeParameter &&
		    cursorkind != CXCursor_UnexposedDecl)
			return 1;
	}
	return 0;
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

	tslen = sb->slen;
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
static int			/* 0 for success, 1 for skipping, -1 for failure */
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

		if (spspelling) {
			ret = prepend_semantic_parent(sb, spspelling);
			clang_disposeString(spspellingstr);
			if (ret < 0)
				break;
		} else {
			ret = 1;
			clang_disposeString(spspellingstr);
			break;
		}

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
	CXSourceLocation		startloc;
	CXSourceLocation		endloc;
	CXSourceRange			currange;
	CXFile				file;
	unsigned int			line;
	unsigned int			column;
	unsigned int			startoffs;
	unsigned int			endoffs;
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
	currange = clang_getCursorExtent(cursor);
	startloc = clang_getRangeStart(currange);
	endloc = clang_getRangeEnd(currange);

	/*
	 * If the location of cursor is NULL, we continue to walk
	 * the AST.
	 */
	if (clang_Range_isNull(currange)) {
		return ret;
	}

	clang_getFileLocation(startloc, &file, &line, &column, &startoffs);
	clang_getFileLocation(endloc, NULL, NULL, NULL, &endoffs);

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
		size_t		i;
		ssize_t		nread;
		size_t		linelen;
		size_t		linebuflen;
		char		*linebuf = NULL;
		struct strbuf_s	*sb = NULL;

		linelen = endoffs - startoffs + 1;
		linebuflen = linelen + 1;	/* Large enough to hold \0 */

		linebuf = (char *)malloc(linebuflen);
		if (!linebuf) {
			goto out;
		}
		linebuf[linelen] = 0;

		/*
		 * Fetch the required line from the source file
		 */
		nread = read_file_data(argsp->srcfilefp,
				       startoffs,
				       linelen,
				       linebuf);
		/* Consider the case of premature EOF or read error */
		if (nread < linelen) {
			ret = CXChildVisit_Break;
			goto cleanup;
		}

		sb = strbuf_open(0);
		if (!sb) {
			ret = CXChildVisit_Break;
			goto cleanup;
		}

		/*
		 * Make sure that we only fetch a line
		 */
		for (i = 0; i < linelen; i++) {
			if (linebuf[i] != '\r' &&
			    linebuf[i] != '\n') {
				continue;
			}
			/*
			 * Do the truncation
			 */
			linebuf[i] = 0;
			break;
		}

		/*
		 * Pass the tag information we gathered to Global
		 */
		if (is_definition(cursor)) {
			int	rc;

			if (strbuf_puts(sb, spellstr) < 0) {
				ret = CXChildVisit_Break;
				goto cleanup;
			}
			rc = fix_tag_semantic_parent(cursor, sb);
			if (rc < 0) {
				ret = CXChildVisit_Break;
				goto cleanup;
			} else if (rc == 1) {
				/*
				 * Skip this symbol
				 */
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
	CXIndex			cxindex = NULL;
	CXTranslationUnit	tu = NULL;
	leveldb_t		*cpdb = NULL;
	char			*cpoptions = NULL;
	const char		*cpdbpath;
	struct visit_args	visitargs;

	assert(param->size >= sizeof(*param));

	cpdbpath = getenv("CPDB_PATH");
	if (cpdbpath) {
		char			*errptr;
		leveldb_options_t	*options;

		options = leveldb_options_create();
		if (!options) {
			param->warning("Insufficient memory creating leveldb options");
			goto out;
		}

		cpdb = leveldb_open(options,
				    cpdbpath,
				    &errptr);

		leveldb_options_destroy(options);

		if (errptr) {
			param->warning("Failed to open database: %s", errptr);
			leveldb_free(errptr);
			cpdb = NULL;
			goto out;
		}
	}
	cxindex = clang_createIndex(0, 0);
	if (!cxindex)
		goto out;

	tu = clang_createTranslationUnitFromSourceFile(cxindex,
						       param->file,
						       0,
						       NULL,
						       0,
						       NULL);
	if (!tu)
		goto out;

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
	if (tu)
		clang_disposeTranslationUnit(tu);
	if (cxindex)
		clang_disposeIndex(cxindex);
	if (cpdb)
		leveldb_close(cpdb);
	if (cpoptions)
		free(cpoptions);
}

/* vim: set noexpandtab ts=8 sw=8 :*/
