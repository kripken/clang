from clang.cindex import CursorKind
from clang.cindex import Cursor
from clang.cindex import Index
from clang.cindex import TranslationUnitSaveError
from clang.cindex import TranslationUnit
from .util import get_cursor
from .util import get_tu
import os

kInputsDir = os.path.join(os.path.dirname(__file__), 'INPUTS')

def test_spelling():
    path = os.path.join(kInputsDir, 'hello.cpp')
    tu = TranslationUnit.from_source(path)
    assert tu.spelling == path

def test_cursor():
    path = os.path.join(kInputsDir, 'hello.cpp')
    tu = get_tu(path)
    c = tu.cursor
    assert isinstance(c, Cursor)
    assert c.kind is CursorKind.TRANSLATION_UNIT

def test_parse_arguments():
    path = os.path.join(kInputsDir, 'parse_arguments.c')
    tu = TranslationUnit.from_source(path, ['-DDECL_ONE=hello', '-DDECL_TWO=hi'])
    spellings = [c.spelling for c in tu.cursor.get_children()]
    assert spellings[-2] == 'hello'
    assert spellings[-1] == 'hi'

def test_reparse_arguments():
    path = os.path.join(kInputsDir, 'parse_arguments.c')
    tu = TranslationUnit.from_source(path, ['-DDECL_ONE=hello', '-DDECL_TWO=hi'])
    tu.reparse()
    spellings = [c.spelling for c in tu.cursor.get_children()]
    assert spellings[-2] == 'hello'
    assert spellings[-1] == 'hi'

def test_unsaved_files():
    tu = TranslationUnit.from_source('fake.c', ['-I./'], unsaved_files = [
            ('fake.c', """
#include "fake.h"
int x;
int SOME_DEFINE;
"""),
            ('./fake.h', """
#define SOME_DEFINE y
""")
            ])
    spellings = [c.spelling for c in tu.cursor.get_children()]
    assert spellings[-2] == 'x'
    assert spellings[-1] == 'y'

def test_unsaved_files_2():
    import StringIO
    tu = TranslationUnit.from_source('fake.c', unsaved_files = [
            ('fake.c', StringIO.StringIO('int x;'))])
    spellings = [c.spelling for c in tu.cursor.get_children()]
    assert spellings[-1] == 'x'

def normpaths_equal(path1, path2):
    """ Compares two paths for equality after normalizing them with
        os.path.normpath
    """
    return os.path.normpath(path1) == os.path.normpath(path2)

def test_includes():
    def eq(expected, actual):
        if not actual.is_input_file:
            return  normpaths_equal(expected[0], actual.source.name) and \
                    normpaths_equal(expected[1], actual.include.name)
        else:
            return normpaths_equal(expected[1], actual.include.name)

    src = os.path.join(kInputsDir, 'include.cpp')
    h1 = os.path.join(kInputsDir, "header1.h")
    h2 = os.path.join(kInputsDir, "header2.h")
    h3 = os.path.join(kInputsDir, "header3.h")
    inc = [(src, h1), (h1, h3), (src, h2), (h2, h3)]

    tu = TranslationUnit.from_source(src)
    for i in zip(inc, tu.get_includes()):
        assert eq(i[0], i[1])

def save_tu(tu):
    """Convenience API to save a TranslationUnit to a file.

    Returns the filename it was saved to.
    """

    # FIXME Generate a temp file path using system APIs.
    base = 'TEMP_FOR_TRANSLATIONUNIT_SAVE.c'
    path = os.path.join(kInputsDir, base)

    # Just in case.
    if os.path.exists(path):
        os.unlink(path)

    tu.save(path)

    return path

def test_save():
    """Ensure TranslationUnit.save() works."""

    tu = get_tu('int foo();')

    path = save_tu(tu)
    assert os.path.exists(path)
    assert os.path.getsize(path) > 0
    os.unlink(path)

def test_save_translation_errors():
    """Ensure that saving to an invalid directory raises."""

    tu = get_tu('int foo();')

    path = '/does/not/exist/llvm-test.ast'
    assert not os.path.exists(os.path.dirname(path))

    try:
        tu.save(path)
        assert False
    except TranslationUnitSaveError as ex:
        expected = TranslationUnitSaveError.ERROR_UNKNOWN
        assert ex.save_error == expected

def test_load():
    """Ensure TranslationUnits can be constructed from saved files."""

    tu = get_tu('int foo();')
    assert len(tu.diagnostics) == 0
    path = save_tu(tu)

    assert os.path.exists(path)
    assert os.path.getsize(path) > 0

    tu2 = TranslationUnit.from_ast_file(filename=path)
    assert len(tu2.diagnostics) == 0

    foo = get_cursor(tu2, 'foo')
    assert foo is not None

    # Just in case there is an open file descriptor somewhere.
    del tu2

    os.unlink(path)

def test_index_parse():
    path = os.path.join(kInputsDir, 'hello.cpp')
    index = Index.create()
    tu = index.parse(path)
    assert isinstance(tu, TranslationUnit)
