from ..models import UnifiedAlert


def event_key(alert: UnifiedAlert) -> tuple[str, str]:
    return alert.status, alert.identity.event_key

