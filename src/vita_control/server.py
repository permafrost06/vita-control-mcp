"""Stdio MCP server: stdout is reserved for MCP protocol messages."""

from mcp.server.fastmcp import FastMCP, Image

from .client import BUTTONS, VitaClient

mcp = FastMCP(
    "vita-control",
    instructions=(
        "Control a PS Vita game over local Wi-Fi. Take a screenshot to observe the game, "
        "then send short timed inputs. Button presses merge with physical controls. "
        "Stick coordinates range from -1 to 1: x positive is right, y positive is down. "
        "Inputs expire after at most 5000 ms. Tools operate only inside the configured game."
    ),
)


@mcp.tool()
def vita_status() -> dict:
    """Check connection and installed game hooks, and list supported button names."""
    with VitaClient.from_env() as client:
        return {**client.info(), "buttons": list(BUTTONS)}


@mcp.tool()
def vita_screenshot() -> Image:
    """Capture the next submitted game frame and return a PNG image for visual reasoning.

    The game must be running and rendering. May take up to two seconds to capture.
    """
    with VitaClient.from_env() as client:
        return Image(data=client.screenshot(), format="png")


@mcp.tool()
def vita_press(
    buttons: list[str],
    duration_ms: int = 100,
    left_stick: list[float] | None = None,
    right_stick: list[float] | None = None,
) -> str:
    """Hold buttons and/or analog sticks for 1-5000 ms, then release before returning.

    buttons: cross, circle, square, triangle, up, down, left, right, l, r, start, select.
    Pass [] for a stick-only action. Each optional stick is [x, y] in [-1, 1];
    [-1, 0] is left, [0, -1] is up, and [0, 0] explicitly centers that stick.
    Unspecified sticks keep physical input. A press merges with physical buttons.
    """
    with VitaClient.from_env() as client:
        client.press(buttons, duration_ms, left_stick, right_stick)
    return f"Input completed and released after {duration_ms} ms."


@mcp.tool()
def vita_release() -> str:
    """Clear injected controls. Physical controls remain active."""
    with VitaClient.from_env() as client:
        client.release()
    return "Injected controls released."


def main() -> None:
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
