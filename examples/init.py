"""Copy to ~/.config/cpymacs/init.py. This example is frontend-independent."""
from datetime import datetime
from cpymacs_plugins import dotemacs

dotemacs.install(api)
api.set_option("line_numbers", True)


def insert_timestamp(editor):
    editor.insert(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))


api.add_command("insert-timestamp", insert_timestamp)
api.bind_key("<f5>", "insert-timestamp")
