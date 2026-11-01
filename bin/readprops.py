import configparser
import subprocess
import os
run_number = os.getenv('GITHUB_RUN_NUMBER', '0')
build_location = os.getenv('BUILD_LOCATION', 'local')

# hydra: local build counter, appended to the version as "-v<n>"
HYDRA_BUILD_FILE = "hydra_build.txt"
# hydra: marks our own builds in the version string ("2.7.23-hydra-v9")
HYDRA_TAG = "-hydra"
# DeviceMetadata.firmware_version is char[18], so at most 17 chars + NUL
MAX_VERSION_LEN = 17


def readHydraBuild(projectDir):
    try:
        with open(os.path.join(projectDir, HYDRA_BUILD_FILE)) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def bumpHydraBuild(projectDir):
    """Increase the build counter by one and return the new value"""
    n = (readHydraBuild(projectDir) or 1) + 1
    with open(os.path.join(projectDir, HYDRA_BUILD_FILE), "w") as f:
        f.write("{}\n".format(n))
    return n

def readProps(prefsLoc):
    """Read the version of our project as a string"""

    config = configparser.RawConfigParser()
    config.read(prefsLoc)
    version = dict(config.items("VERSION"))
    verObj = dict(
        short="{}.{}.{}".format(version["major"], version["minor"], version["build"]),
        long="unset",
        deb="unset",
    )

    # Try to find current build SHA if if the workspace is clean.  This could fail if git is not installed
    try:
        # Pin abbreviation length to keep local builds and CI matching (avoid auto-shortening)
        sha = (
            subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"])
            .decode("utf-8")
            .strip()
        )
        isDirty = (
            subprocess.check_output(["git", "diff", "HEAD"]).decode("utf-8").strip()
        )
        suffix = sha
        # if isDirty:
        #     # short for 'dirty', we want to keep our verstrings source for protobuf reasons
        #     suffix = sha + "-d"
        verObj["long"] = "{}.{}".format(verObj["short"], suffix)
        hydraBuild = readHydraBuild(os.path.dirname(os.path.abspath(prefsLoc)))
        if hydraBuild is not None:
            # "2.7.23-hydra-v9": the commit sha stays in "deb" and in git, firmware_version only fits 17 chars
            verObj["long"] = "{}{}-v{}".format(verObj["short"], HYDRA_TAG, hydraBuild)[:MAX_VERSION_LEN]
        verObj["deb"] = "{}.{}~{}{}".format(verObj["short"], run_number, build_location, sha)
    except:
        # print("Unexpected error:", sys.exc_info()[0])
        # traceback.print_exc()
        verObj["long"] = verObj["short"]
        verObj["deb"] = "{}.{}~{}".format(verObj["short"], run_number, build_location)

    # print("firmware version " + verStr)
    return verObj


# print("path is" + ','.join(sys.path))