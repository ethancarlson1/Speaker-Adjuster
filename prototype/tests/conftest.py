import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from roomeq import roomsim  # noqa: E402


@pytest.fixture(scope="session")
def sim():
    """Default simulated club (RIRs are computed once per test session)."""
    return roomsim.SimulatedRoom()
