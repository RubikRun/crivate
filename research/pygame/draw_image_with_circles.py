
# Import and initialize the pygame library
import pygame
import random

# Returns a random point on the screen
def get_rand_point_on_screen(screen_size):
    return [random.randint(0, screen_size[0] - 1), random.randint(0, screen_size[1] - 1)]

# Samples a position on the screen inside the image,
# as if the image is scaled to match the screen size.
# Returns the color of the pixel that sits on the given position on the screen.
def sample_image(image: pygame.Surface, position: list[int, int], screen_size: list[int, int]):
    image_size = [image.get_width(), image.get_height()]
    position_image = [
        int(position[0] * image_size[0] / screen_size[0]),
        int(position[1] * image_size[1] / screen_size[1])
    ]
    return image.get_at(position_image)

pygame.init()

# Change window's title
pygame.display.set_caption('Crivate')
# Change window's icon
logo = pygame.image.load('logo.png')
pygame.display.set_icon(logo)

# Set up the drawing window
screen_size = [1920 / 3, 1080 / 3]
screen = pygame.display.set_mode(screen_size)
# Fill the background with white
screen.fill((255, 255, 255))

image = pygame.image.load('original_images/hires00.jpg')
radius = 3

# Run until the user asks to quit
running = True
while running:
    # Did the user click the window close button?
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
    # Put the circle on a random position on the screen
    center = get_rand_point_on_screen(screen_size)
    # Draw a circle with the color of the image in the position where the circle sits.
    pygame.draw.circle(screen, sample_image(image, center, screen_size), center, radius)
    # Update the display
    pygame.display.update()

# Done! Time to quit.
pygame.quit()