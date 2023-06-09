# Simple pygame program

# Import and initialize the pygame library
import pygame
pygame.init()

# Change window's title
pygame.display.set_caption('Crivate')
# Change window's icon
logo = pygame.image.load('logo.png')
pygame.display.set_icon(logo)

# Set up the drawing window
screen = pygame.display.set_mode([1700, 900])

# Run until the user asks to quit
running = True
while running:
    # Did the user click the window close button?
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
    # Fill the background with white
    screen.fill((255, 255, 255))
    # Draw a solid blue circle in the center
    pygame.draw.circle(screen, (0, 0, 255), (250, 250), 75)
    # Update the display
    pygame.display.update()

# Done! Time to quit.
pygame.quit()