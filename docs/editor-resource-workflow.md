# Materials and texture resources

Use the existing **Scene Controls** panel. Click **Material**, or choose
**Go to → Materials and resources**. The controls use the existing Dear ImGui UI.

## Replace a texture

1. Select a surface and choose its material.
2. Expand **Texture slots** and the slot you want to edit.
3. Enter an **Image path**, then click **Replace from file** or **Import image**.
4. Use the same slot's UV mapping and sampler controls as needed.

**Clear slot** removes that binding. Other materials and the resource catalog
keep their resources. **Existing image** lets you bind a retained source texture.
Unavailable controls show the reason next to the affected slot.

## Delete an unused resource

The **Resources** section works without selecting a surface or source material.
Choose a row in **Resource catalog**, then click **Delete unused resource**.

| Label | Meaning |
| --- | --- |
| Imported texture, image or sampler | The asset's retained source definition. |
| Evaluated texture, image or sampler | The corresponding definition used by the renderer. |
| Sampled texture | A runtime texture used for a color or data role. |

Deletion checks current materials, source records, roles, caches, animation rest
state and known providers. A disabled button shows the blocking reason. A
resource can remain in use even when its surface is hidden or unselected.

Deleting an unused source texture also removes its proved orphan resources in
the same operation. Shared images, samplers and runtime roles remain available.
Choosing a catalog row only changes inspection; it does not assign a texture.

## Undo, redo and save

Use **Undo (Ctrl+Z)** and **Redo (Ctrl+Y)**. Texture imports, binding changes and
resource deletion share the existing history. Original import history preserves
resource identities when undo and redo cross a later resource deletion.

**Make unique** creates a material for the selected surface. To delete it later,
assign the surface back to another material, choose the unused entry in
**Material catalog**, and click **Delete unused material**. Material creation,
factor edits, assignments and deletion share the same Undo/Redo history. Redo
restores the original material identity when replay crosses its creation.

Choose **Go to → Import / save** to use **Save scene** or **Save As**. Both glTF
and GLB exports remove retired resource holes and remap the surviving references.
Live editor resource IDs remain stable.

Undo history retains selected GPU images until their last history or reader
owner is released. Shared allocations remain alive while another alias uses
them. Retired source records currently retain their CPU image payloads; CPU
payload reclamation and additional external renderer acceptance remain tracked
in #108 and #121.
