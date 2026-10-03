# This is a sample commands.py.  You can add your own commands here.
#
# All the default commands with complete documentation: run
# `ranger --copy-config=commands_full` to generate commands_full.py.  Do NOT add them all here, or you may end up with defunct
# commands when upgrading ranger.

# A simple command for demonstration purposes follows.
# -----------------------------------------------------------------------------

from __future__ import (absolute_import, division, print_function)

# You can import any python module as needed.
import os

# You always need to import ranger.api.commands here to get the Command class:
from ranger.api.commands import Command


# Any class that is a subclass of "Command" will be integrated into ranger as a
# command.  Try typing ":my_edit<ENTER>" in ranger!
class my_edit(Command):
    # The so-called doc-string of the class will be visible in the built-in
    # help that is accessible by typing "?c" inside ranger.
    """:my_edit <filename>

    A sample command for demonstration purposes that opens a file in an editor.
    """

    # The execute method is called when you run this command in ranger.
    def execute(self):
        # self.arg(1) is the first (space-separated) argument to the function.
        # This way you can write ":my_edit somefilename<ENTER>".
        if self.arg(1):
            # self.rest(1) contains self.arg(1) and everything that follows
            target_filename = self.rest(1)
        else:
            # self.fm is a ranger.core.filemanager.FileManager object and gives
            # you access to internals of ranger.
            # self.fm.thisfile is a ranger.container.file.File object and is a
            # reference to the currently selected file.
            target_filename = self.fm.thisfile.path

        # This is a generic function to print text in ranger.
        self.fm.notify("Let's edit the file " + target_filename + "!")

        # Using bad=True in fm.notify allows you to print error messages:
        if not os.path.exists(target_filename):
            self.fm.notify("The given file does not exist!", bad=True)
            return

        # This executes a function from ranger.core.acitons, a module with a
        # variety of subroutines that can help you construct commands.
        # Check out the source, or run "pydoc ranger.core.actions" for a list.
        self.fm.edit_file(target_filename)

    # The tab method is called when you press tab, and should return a list of
    # suggestions that the user will tab through.
    # tabnum is 1 for <TAB> and -1 for <S-TAB> by default
    def tab(self, tabnum):
        # This is a generic tab-completion function that iterates through the
        # content of the current directory.
        return self._tab_directory_content()


# 修复 ranger 1.9.4 自带 :trash (dT) 的崩溃:
# 原实现把文件名字符串传给 fm.execute_file(), 而它需要 File 对象 (要读取 f.path),
# 结果报 AttributeError: 'str' object has no attribute 'path'。
# 这里同名覆盖, 逻辑不变, 只是改为传入 File 对象。
from ranger.config.commands import trash as _builtin_trash  # noqa: E402


class trash(_builtin_trash):
    """:trash

    将选中的文件 (或参数中的文件) 移到回收站 (rifle 中 label 为 trash 的规则, 即 trash-put)。
    删除多个文件或非空目录时需要确认。
    """

    def execute(self):
        import shlex
        from functools import partial
        from ranger.container.file import File

        def is_directory_with_files(path):
            return os.path.isdir(path) and not os.path.islink(path) and len(os.listdir(path)) > 0

        if self.rest(1):
            names = shlex.split(self.rest(1))
            files = [File(os.path.abspath(name)) for name in names]
            many_files = len(names) > 1 or is_directory_with_files(names[0])
        else:
            cwd = self.fm.thisdir
            tfile = self.fm.thisfile
            if not cwd or not tfile:
                self.fm.notify("Error: no file selected for deletion!", bad=True)
                return
            files = self.fm.thistab.get_selection()
            names = [f.relative_path for f in files]
            many_files = bool(cwd.marked_items) or is_directory_with_files(tfile.path)

        confirm = self.fm.settings.confirm_on_delete
        if confirm != 'never' and (confirm != 'multiple' or many_files):
            self.fm.ui.console.ask(
                "Confirm deletion of: %s (y/N)" % ', '.join(names),
                partial(self._question_callback, files),
                ('n', 'N', 'y', 'Y'),
            )
        else:
            self.fm.execute_file(files, label='trash')
