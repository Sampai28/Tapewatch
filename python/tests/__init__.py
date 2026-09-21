# Standard-library unittest, not pytest. The Python side of Tapewatch has no
# third-party dependencies, and adding one solely to run the tests would mean
# the suite cannot run anywhere the pipeline can.
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
