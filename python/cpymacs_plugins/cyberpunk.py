"""An original cyberpunk-inspired palette, not copied Emacs theme source."""
COLORS = [
    0xE4E4EF,  # Text
    0x181818,  # Background
    0x878787,  # Comments
    0xFFAF00,  # Decorators and annotations
    0xD7FF87,  # Strings
    0xFF5FAF,  # Keywords
    0x5FD7FF,  # Functions, builtins and tags
    0xFFAF5F,  # Numbers
    0x4E3A65,  # Region
    0x303030,  # Mode line
]


def install(api):
    api.set_theme("cyberpunk", COLORS)
