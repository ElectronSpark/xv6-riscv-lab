
void hyperv_input_init(void) {}
void hyperv_input_intr(void) {}
void hyperv_storvsc_init(void) {}
void hyperv_netvsc_init(void) {}
void hyperv_video_dirty(uint32 x, uint32 y, uint32 w, uint32 h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

int hyperv_video_get_status(struct hyperv_video_status *status)
{
    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    return -ENODEV;
}

