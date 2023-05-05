# Import and initialize the pygame library
import pygame
pygame.init()

image = pygame.image.load('logo.png')
for i in range(32):
    for j in range(32):
        print('get_at() = ', image.get_at([i,j]))

# Done! Time to quit.
pygame.quit()