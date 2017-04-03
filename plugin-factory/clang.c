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
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
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
 * Check if the cursor points to a definition
 */
static int
is_definition(CXCursor cursor)	/* Cursor */
{
	return clang_isCursorDefinition(cursor);
}

/*
 * AST walker routine
 *
 * If we found definition and reference tags, we pass
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

	argsp = (const struct visit_args *)data;
	param = argsp->param;

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
	
	if (is_definition(cursor)) {
		ssize_t	nread;
		ssize_t	linelen;
		size_t	linebuflen = 0;
		char	*linebuf = NULL;

		/*
		 * Fetch the required line from the source file
		 */
		nread = read_line_no(argsp->srcfilefp,
				     line,
				     &linebuf,
				     &linebuflen);
		if (nread <= 0) {
			free_line_buf(linebuf);
			goto out;
		}
		linelen = nread;

		fprintf(stderr,
			"Definition [%s] [%u] [%s] [%s]\n",
			spellstr,
			line,
			pathstr,
			linebuf);

		/*
		 * Pass the tag information we gathered to Global
		 */
		param->put(PARSER_DEF,
			   spellstr,
			   line,
			   pathstr,
			   linebuf,
			   param->arg);

		free_line_buf(linebuf);
	}

out:
	clang_disposeString(pathcxstr);
	clang_disposeString(spellcxstr);
	return CXChildVisit_Recurse;
}

/*
 * Main parser plugin routine
 *
 * This routine passes definition tags and reference tags
 * to Global at once when these tags are discovered.
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
	 * once we found them.
	 */
	clang_visitChildren(clang_getTranslationUnitCursor(tu),
			    visit_children,
			    &visitargs);

	fclose(visitargs.srcfilefp);
out:
	clang_disposeTranslationUnit(tu);
	clang_disposeIndex(cxindex);
}
