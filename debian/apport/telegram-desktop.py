import os, os.path
from apport.hookutils import attach_file_if_exists


def add_info(report, ui=None):
    data_home = os.path.expanduser(os.getenv("XDG_DATA_HOME") or "~/.local/share")
    logfile = os.path.join(data_home, "TelegramDesktop/log.txt")
    attach_file_if_exists(report, logfile, key="TelegramDesktopLogFile")
