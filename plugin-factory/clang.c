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

#include <sys/types.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <clang-c/Index.h>

#include "parser.h"

struct visitor_args {
	FILE				*srcfilefp;
	const struct parser_param	*param;
};

static void
free_line_buf(char *linebuf)
{
	free(linebuf);
}

static ssize_t
read_line_no(
	FILE		*srcfilefp,
	unsigned int	line,
	char		**linep,
	size_t		*lenp)
{
	unsigned int	i;
	ssize_t		nread = 0;

	assert(line);

	rewind(srcfilefp);
	for (i = 0; i < line; ++i) {
		nread = getline(linep, lenp, srcfilefp);
		if (nread <= 0)
			break;
	}

	return nread;
}

static bool
is_definition(CXCursor cursor)
{
	return clang_isCursorDefinition(cursor);
}

enum CXChildVisitResult
visit_children(
	CXCursor	cursor,
	CXCursor	parent,
	CXClientData	data)
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
	const struct visitor_args	*argsp;
	const struct parser_param	*param;

	argsp = (const struct visitor_args *)data;
	param = argsp->param;
	loc = clang_getCursorLocation(cursor);
	clang_getSpellingLocation(loc, &file, &line, &column, &offs);

	pathcxstr = clang_getFileName(file);
	spellcxstr = clang_getCursorSpelling(cursor);

	pathstr = clang_getCString(pathcxstr);
	spellstr = clang_getCString(spellcxstr);

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

void
parser(const struct parser_param *param)
{
	CXIndex			cxindex;
	CXTranslationUnit	tu;
	struct visitor_args	args;

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

	args.srcfilefp = fopen(param->file, "r");
	if (!args.srcfilefp)
		goto out;
	args.param = param;

	clang_visitChildren(clang_getTranslationUnitCursor(tu),
			    visit_children,
			    &args);

	fclose(args.srcfilefp);
out:
	/* Read output of ctags command. */
	clang_disposeTranslationUnit(tu);
	clang_disposeIndex(cxindex);
}
