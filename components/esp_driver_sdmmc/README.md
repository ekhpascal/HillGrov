# esp_driver_sdmmc -- HillGrow override of ESP-IDF v6.0.1

This directory is a copy of `components/esp_driver_sdmmc` from ESP-IDF **v6.0.1**
(commit `8c19b156084a0753687347cca1f5355782893533`), minus `test_apps/` and IDF's README, with **one** change:
`sd_host_isr()` in `src/sd_host_sdmmc.c`. Every other file is byte-identical to IDF v6.0.1.

## RE-CHECK ON EVERY IDF UPGRADE

`CMakeLists.txt` stops the configure on any IDF version other than 6.0.1. On an upgrade:

1. Check whether the new IDF's `sd_host_isr()` still looks up `ctlr->slot[ctlr->cur_slot_id]` without a NULL check
   and routes SDIO IO interrupts through that slot (see "Why" below).
2. If upstream fixed it: delete this directory and drop the `CONFIG_HILLGROW_PANEL_SD` caveat in
   `components/panel_ui/Kconfig`.
3. If not: re-copy the new IDF's component here, re-apply the diff below, re-run
   `diff -r <idf>/components/esp_driver_sdmmc components/esp_driver_sdmmc -x test_apps -x README.md` (it must show only
   the diff below plus the pin block at the top of `CMakeLists.txt`), and update the version pin.

## Why

The P4 master's one SDMMC controller carries two slots: esp_hosted's link to the C6 on slot 1 and, transiently, the
panel's microSD card on slot 0 (`components/panel_ui/pnl_sd.c`). In IDF v6.0.1:

- `sd_host_isr()` (`src/sd_host_sdmmc.c`) starts with `slot = ctlr->slot[ctlr->cur_slot_id]`, with no NULL check, and
  then dereferences `slot` for the DMA refill, `slot->cbs.on_trans_done` and `slot->cbs.on_io_interrupt`.
- `cur_slot_id` is written only by `sd_host_slot_sdmmc_do_transaction()` (`src/sd_trans_sdmmc.c`).
- `sd_host_controller_remove_sdmmc_slot()` sets `ctlr->slot[id] = NULL` and frees the slot but never resets
  `cur_slot_id`.
- esp_hosted's read thread parks in `sdmmc_io_wait_int()` -> `sd_host_slot_sdmmc_io_int_wait()`, which enables
  `SDMMC_INTMASK_IO_SLOT1` without running a transaction.

So after a card unmount (or a failed mount's cleanup) the last transaction was on slot 0, `cur_slot_id` is still 0 and
`slot[0]` is NULL. The C6's next SDIO interrupt enters the ISR with `slot == NULL` and faults on
`slot->cbs.on_io_interrupt`: the master panics. The window lasts until esp_hosted's next slot-1 transaction, and the
read thread normally sits in that very wait.

Second defect, same line: while slot 0 is still registered, a slot-1 IO interrupt that arrives after a slot-0
transaction is dispatched to **slot 0's** `on_io_interrupt`. In this build that is latent -- nothing registers per-slot
callbacks (`sd_host_slot_register_event_callbacks()` has no caller in IDF v6.0.1 or esp_hosted), and the wake
esp_hosted depends on is the controller-wide `io_intr_sem`, given before the callback -- so it cannot stall esp_hosted
today, but it is fixed with the rest.

## The change

- Transfer-done events and the DMA refill still go to the slot of the last transaction (`cur_slot_id`), now only when
  that slot is still registered.
- SDIO IO interrupts are dispatched per slot, to the slot whose `SDMMC_INTMASK_IO_SLOTn` bit is pending, only when that
  slot is registered. The controller-wide `io_intr_sem` give is unchanged.

Not addressed (not reachable here): the ISR reads the slot pointer without the controller spinlock, so an interrupt
racing a slot removal on the other core could read the slot's callback fields just after the free. The freed memory
stays mapped and no callbacks are registered, so that read cannot fault.

The component also builds for the ESP32 (master DevKitC fallback, zone, rescue: `CONFIG_SOC_SDMMC_HOST_SUPPORTED=y`),
where the change is inert.

## How the override is resolved

`components/` is in `EXTRA_COMPONENT_DIRS` for master, zone and rescue. IDF v6.0.1 orders component sources
`idf_components` < `project_managed_components` < `project_extra_components` < `project_components`
(`tools/cmake/build.cmake`, `idf_build_component`), and a component added later with the same name replaces the
earlier one (`tools/cmake/component.cmake`, `__component_add`, which records the replaced directory as
`COMPONENT_OVERRIDEN_DIR`). Proof that this copy is the one linked, per build:
- the configure log's `-- Component paths:` line lists `C:/Projects/HillGrov/components/esp_driver_sdmmc` (and no IDF
  `esp_driver_sdmmc`);
- `build*/project_description.json` gives `build_component_info.esp_driver_sdmmc.dir` =
  `C:/Projects/HillGrov/components/esp_driver_sdmmc`;
- `build*/compile_commands.json` compiles `C:\Projects\HillGrov\components\esp_driver_sdmmc\src\sd_host_sdmmc.c`
  into `esp-idf/esp_driver_sdmmc/libesp_driver_sdmmc.a`, the archive the map file links `sd_host_isr` from.

## The exact diff against IDF v6.0.1

(The only other difference is the version-pin block at the top of `CMakeLists.txt`.)

```diff
--- a/components/esp_driver_sdmmc/src/sd_host_sdmmc.c
+++ b/components/esp_driver_sdmmc/src/sd_host_sdmmc.c
@@ -788,6 +788,9 @@
 static void sd_host_isr(void *arg)
 {
     sd_host_sdmmc_ctlr_t *ctlr = (sd_host_sdmmc_ctlr_t *)arg;
+    /* HILLGROW OVERRIDE (see ../README.md): transfer events belong to the slot of the last transaction, which may since
+     * have been removed (slot[] NULL, cur_slot_id never reset); SDIO IO interrupts belong to the slot whose IO bit is set,
+     * which need not be the last transaction's. Every slot dereference below is NULL-guarded. */
     sd_host_sdmmc_slot_t *slot = ctlr->slot[ctlr->cur_slot_id];
 
     sd_host_sdmmc_event_t event = {};
@@ -803,7 +806,7 @@
 
     if (dma_pending & SDMMC_LL_EVENT_DMA_NI) {
         // refill DMA descriptors
-        size_t free_desc = sd_host_get_free_descriptors_count(slot);
+        size_t free_desc = slot ? sd_host_get_free_descriptors_count(slot) : 0;
         if (free_desc > 0) {
             sd_host_fill_dma_descriptors(slot, free_desc);
             sd_host_dma_resume(slot);
@@ -815,7 +818,7 @@
 
     if (pending != 0 || dma_pending != 0) {
         xQueueSendFromISR(ctlr->event_queue, &event, &higher_priority_task_awoken);
-        if (slot->cbs.on_trans_done) {
+        if (slot && slot->cbs.on_trans_done) {
             sd_host_evt_data_t edata = {};
             if (slot->cbs.on_trans_done(&slot->drv, &edata, slot->user_data)) {
                 need_yield |= true;
@@ -828,10 +831,16 @@
         // disable the interrupt (no need to clear here, this is done in sdmmc_host_io_int_wait)
         sdmmc_ll_enable_interrupt(ctlr->hal.dev, sdio_pending, false);
         xSemaphoreGiveFromISR(ctlr->io_intr_sem, &higher_priority_task_awoken);
-        if (slot->cbs.on_io_interrupt) {
-            sd_host_evt_data_t edata = {};
-            if (slot->cbs.on_io_interrupt(&slot->drv, &edata, slot->user_data)) {
-                need_yield |= true;
+        for (int i = 0; i < SOC_SDMMC_NUM_SLOTS; i++) {
+            if (!(sdio_pending & (i == 0 ? SDMMC_INTMASK_IO_SLOT0 : SDMMC_INTMASK_IO_SLOT1))) {
+                continue;
+            }
+            sd_host_sdmmc_slot_t *io_slot = ctlr->slot[i];
+            if (io_slot && io_slot->cbs.on_io_interrupt) {
+                sd_host_evt_data_t edata = {};
+                if (io_slot->cbs.on_io_interrupt(&io_slot->drv, &edata, io_slot->user_data)) {
+                    need_yield |= true;
+                }
             }
         }
     }
```
