"""Optional Gymnasium navigation interface for the native BIM simulator."""

from .environment import BimNavigationEnv
from ._native import NavigationProtocolError, NavigationRequestError
from .policy import baseline_action

__all__ = ["BimNavigationEnv", "NavigationProtocolError", "NavigationRequestError", "baseline_action"]
