"""Where the coordinator keeps job files: one folder per job id, ids restricted to scheduler ids."""

import re
import shutil

from . import config

JOB_ID = re.compile(r"^[0-9a-f]{32}$")
RESULT_NAMES = ("s11.s1p", "summary.txt", "settings.json", "run.log")
# a --keep-fields job also returns what makes the folder a saved run: the manifest, the board, and the field
# files, flattened (a blob name is a plain file name): fields.NAME stands for fields/NAME
FIELD_RESULT_NAMES = ("run.json", "board.json")
FIELD_FILE = re.compile(r"^fields\.[a-z0-9_]{1,24}\.(f16|json)$")


def is_result_name(name):
    return name in RESULT_NAMES or name in FIELD_RESULT_NAMES or bool(FIELD_FILE.match(name))


def root():
    return config.home() / "jobs"


def job_dir(job_id):
    if not JOB_ID.match(str(job_id)):
        raise ValueError("bad job id")
    return root() / job_id


def input_dir(job_id):
    return job_dir(job_id) / "input"


def result_dir(job_id):
    return job_dir(job_id) / "result"


def forget(job_id):
    shutil.rmtree(str(job_dir(job_id)), ignore_errors=True)
