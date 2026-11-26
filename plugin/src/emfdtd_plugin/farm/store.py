"""Where the coordinator keeps job files: one folder per job id, ids restricted to scheduler ids."""

import re
import shutil

from . import config

JOB_ID = re.compile(r"^[0-9a-f]{32}$")
RESULT_NAMES = ("s11.s1p", "summary.txt", "settings.json", "run.log")


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
