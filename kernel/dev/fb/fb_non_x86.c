
void fb_pci_init(uint8 bus, uint8 dev, uint8 func)
{
    (void)bus; (void)dev; (void)func;
}

int fb_detected(void) { return 0; }

void fb_get_resolution(uint32 *xres, uint32 *yres)
{
    if (xres)
        *xres = 0;
    if (yres)
        *yres = 0;
}

void fbdevinit(void) {}

int fb_gpu_register_render_node(void) { return -ENODEV; }

void fb_panic_screen(const char *text) { (void)text; }

