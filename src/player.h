#include "physics.h"

/* ------------------------- *
   -        Player         -
 * ------------------------- */

// first person player : a capsule that sets its own velocity (rotation locked)
// and jumps off a ground raycast, with a free fly camera on F.

const float PLAYER_RADIUS  = .4f;
const float PLAYER_HEIGHT  = 1.f; // cylinder part : 1.8 total
const float PLAYER_MASS    = 75.f;
const float WALK_SPEED     = 4.5f;
const float SPRINT_SCALE   = 1.9f;
const float JUMP_SPEED     = 5.5f;
const float EYE_HEIGHT     = .75f; // above the capsule center

struct Player
{
	struct Slot { uint block; uint count; };

	Camera camera; // eyes
	uint   body;   // capsule phys_id (0 = none)
	bool   flycam; // F : free camera instead of the capsule
	float  bob;    // walk bob phase
	bool   grounded;

	// inventory and the first-person arms
	Slot   hotbar[9];
	Slot   storage[27];
	Slot   craft[4];     // the 2 x 2 inventory grid
	Slot   armor[4];
	Slot   offhand;
	uint   selected;
	Slot   drag_stack;   // carried on the cursor in the inventory
	int    drag_source;
	bool   inventory_open;
	bool   has_inventory;
	uint   arm_mesh;
	uint   hand_mesh; // first-person arm, excluded from the sun depth pass
	float  arm_swing;
	bool   voxel_mode;
	bool   third_person;
	vec3   voxel_pos; // feet
	vec3   voxel_vel;
	bool   voxel_grounded;

	void init(Physics* phys, vec3 spawn)
	{
		body = phys->add_capsule(spawn, PLAYER_RADIUS, PLAYER_HEIGHT, PLAYER_MASS);
		if (!body) return;

		btRigidBody* b = phys->bodies[body].body;
		b->setAngularFactor(0); // never tips over
		b->setFriction(.9f);

		camera.position = spawn + vec3(0, EYE_HEIGHT, 0);
		camera.yaw      = -PI / 2; // facing -z, into the scene
		camera.pitch    = 0;
		camera.update_dir(0, 0, 0);

		console->add_entry("Player spawned : F swaps to the fly camera", SUCCESS, PHYS);
	}

	void init_voxel(vec3 spawn)
	{
		voxel_mode = true;
		body = 0;
		voxel_pos = spawn;
		voxel_vel = vec3(0);
		voxel_grounded = false;
		camera.position = spawn + vec3(0, 1.62f, 0);
		camera.yaw = -PI / 2;
		camera.pitch = -.08f;
		camera.update_dir(0, 0, 0);
	}

	void start_inventory(GameRenderer* renderer, const uint* blocks, const uint* counts, uint n)
	{
		has_inventory = true;
		selected = 0;
		inventory_open = false;
		for (uint i = 0; i < 9; i++)
		{
			hotbar[i].block = (i < n) ? blocks[i] : 0;
			hotbar[i].count = (i < n) ? counts[i] : 0;
		}
		for (uint i = 0; i < 27; i++) storage[i] = {};
		for (uint i = 0; i < 4; i++) { craft[i] = {}; armor[i] = {}; }
		offhand = {};
		drag_stack = {};
		drag_source = -1;
		arm_mesh = renderer->add_mesh(MeshFactory::generate_cube(1.f));
		hand_mesh = renderer->add_mesh(MeshFactory::generate_cube(1.f));
		renderer->no_shadow_mesh = hand_mesh;
	}

	Slot* slot_at(int index)
	{
		if (index < 0) return NULL;
		if (index < 9) return &hotbar[index];
		if (index < 36) return &storage[index - 9];
		if (index < 40) return &craft[index - 36];
		if (index < 44) return &armor[index - 40];
		if (index == 44) return &offhand;
		return NULL;
	}

	// minecraft-style cursor stack : pick one slot up, then click another to
	// merge or swap. release over a different slot behaves like the second click
	void slot_click(int index)
	{
		Slot* slot = slot_at(index);
		if (!slot) return;

		if (drag_stack.block == 0 || drag_stack.count == 0)
		{
			if (slot->block == 0 || slot->count == 0) return;
			drag_stack = *slot;
			drag_source = index;
			*slot = {};
			return;
		}

		if (slot->block == 0 || slot->count == 0)
		{
			*slot = drag_stack;
			drag_stack = {};
			drag_source = -1;
			return;
		}

		if (slot->block == drag_stack.block)
		{
			uint room = 64 - slot->count;
			uint move = (drag_stack.count < room) ? drag_stack.count : room;
			slot->count += move;
			drag_stack.count -= move;
			if (drag_stack.count == 0) { drag_stack = {}; drag_source = -1; }
			return;
		}

		Slot swap = *slot;
		*slot = drag_stack;
		drag_stack = swap;
		drag_source = index;
	}

	bool add_item(uint block, uint count = 1)
	{
		if (!has_inventory || block == 0 || count == 0) return false;

		uint remaining = count;
		Slot* all[36];
		for (uint i = 0; i < 9; i++) all[i] = &hotbar[i];
		for (uint i = 0; i < 27; i++) all[9 + i] = &storage[i];

		// top up existing stacks first
		for (uint i = 0; i < 36 && remaining; i++)
		{
			Slot* s = all[i];
			if (s->block != block || s->count >= 64) continue;
			uint take = 64 - s->count;
			if (take > remaining) take = remaining;
			s->count += take;
			remaining -= take;
		}

		// then fill empty slots
		for (uint i = 0; i < 36 && remaining; i++)
		{
			Slot* s = all[i];
			if (s->block != 0 && s->count != 0) continue;
			uint take = remaining > 64 ? 64 : remaining;
			s->block = block;
			s->count = take;
			remaining -= take;
		}

		return remaining == 0;
	}

	bool take_item(uint block, uint amount)
	{
		Slot* all[36];
		for (uint i = 0; i < 9; i++) all[i] = &hotbar[i];
		for (uint i = 0; i < 27; i++) all[9 + i] = &storage[i];

		for (uint i = 0; i < 36; i++)
			if (all[i]->block == block && all[i]->count >= amount)
			{
				all[i]->count -= amount;
				if (all[i]->count == 0) all[i]->block = 0;
				return true;
			}
		return false;
	}

	// first-person arms : sleeve, forearm and a fist, entering from the lower corners
	void draw_arms(GameRenderer* renderer)
	{
		if (!has_inventory || inventory_open || flycam || third_person || !arm_mesh) return;

		Camera& c = camera;
		mat3 aim;
		aim[0] = c.right;
		aim[1] = c.up;
		aim[2] = -c.front;
		mat4 view = glm::translate(mat4(1), c.position) * mat4(aim);

		float bob_y = sinf(bob * 4.f) * .010f;
		float sway  = sinf(bob * 2.f) * .008f;
		float kick  = arm_swing * .035f;
		Instance inst[2] = {};
		vec3 skin   = vec3(.86f, .65f, .47f);
		vec3 sleeve = vec3(.20f, .55f, .72f);

		// one arm cuboid, exactly Steve's 4 x 12 x 4 pixels (0.25 x 0.75 x 0.25 m),
		// with the upper 4 px sleeve. no fist, cuff or extra hand blocks : the
		// vanilla view model is one cuboid, the texture supplies the sleeve
		mat4 pivot = view * glm::translate(mat4(1), vec3(.40f + sway, -.42f + bob_y, -.98f + kick))
		           * glm::rotate(mat4(1), .18f, vec3(0, 1, 0))
		           * glm::rotate(mat4(1), 1.48f, vec3(1, 0, 0))
		           * glm::scale(mat4(1), vec3(.80f));

		inst[0] = material(sleeve, 0.f, .72f);
		// the sleeve top sits just inside the skin cap instead of coplanar with it,
		// which was z-fighting on the back of the first-person arm
		inst[0].model = pivot * glm::translate(mat4(1), vec3(0, .235f, 0))
		              * glm::scale(mat4(1), vec3(.26f, .24f, .26f));

		inst[1] = material(skin, 0.f, .82f);
		inst[1].model = pivot * glm::scale(mat4(1), vec3(.25f, .75f, .25f));

		renderer->buffer(MODE_STATIC_UV).append_instances(hand_mesh, 2, inst);
	}

	// third-person Steve : head, hair, eyes, torso, sleeves, forearms and legs
	void draw_steve(GameRenderer* renderer)
	{
		if (!third_person || !has_inventory || !arm_mesh) return;

		vec3 feet = voxel_mode ? voxel_pos : camera.position - vec3(0, 1.62f, 0);
		mat4 base = glm::translate(mat4(1), feet)
		          * glm::rotate(mat4(1), PI * .5f - camera.yaw, vec3(0, 1, 0));

		vec3 skin   = vec3(.86f, .65f, .47f);
		vec3 shirt  = vec3(.20f, .55f, .72f);
		vec3 pants  = vec3(.16f, .22f, .48f);
		vec3 shoes  = vec3(.22f, .22f, .24f);
		vec3 hair   = vec3(.28f, .18f, .10f);
		vec3 eye_w  = vec3(.90f, .92f, .90f);
		vec3 iris   = vec3(.18f, .32f, .58f);
		vec3 mouth  = vec3(.34f, .20f, .16f);

		// Steve's model, in 1/16 m pixels : head 8^3, torso 8 x 12 x 4,
		// each arm and leg 4 x 12 x 4. the total height is 32 px = 2 m
		struct Part { vec3 color; vec3 pos; vec3 scale; };
		const Part parts[] = {
			// legs, shoes
			{ pants, vec3(-.125f, .375f, 0), vec3(.25f, .75f, .25f) },
			{ pants, vec3( .125f, .375f, 0), vec3(.25f, .75f, .25f) },
			{ shoes, vec3(-.125f, .04f, .02f), vec3(.25f, .08f, .29f) },
			{ shoes, vec3( .125f, .04f, .02f), vec3(.25f, .08f, .29f) },
			// torso
			{ shirt, vec3(0, 1.125f, 0), vec3(.50f, .75f, .25f) },
			// arms : 4 px sleeve at the shoulder, 8 px skin below
			{ shirt, vec3(-.375f, 1.375f, 0), vec3(.25f, .25f, .25f) },
			{ shirt, vec3( .375f, 1.375f, 0), vec3(.25f, .25f, .25f) },
			{ skin,  vec3(-.375f, 1.000f, 0), vec3(.25f, .50f, .25f) },
			{ skin,  vec3( .375f, 1.000f, 0), vec3(.25f, .50f, .25f) },
			// head, hair cap, hair back
			{ skin,  vec3(0, 1.75f, 0), vec3(.50f, .50f, .50f) },
			{ hair,  vec3(0, 1.98f, 0), vec3(.54f, .10f, .54f) },
			{ hair,  vec3(0, 1.80f, -.24f), vec3(.50f, .30f, .06f) },
			// face : whites, irises, nose, mouth
			{ eye_w, vec3(-.105f, 1.78f, .255f), vec3(.10f, .06f, .03f) },
			{ eye_w, vec3( .105f, 1.78f, .255f), vec3(.10f, .06f, .03f) },
			{ iris,  vec3(-.105f, 1.78f, .272f), vec3(.05f, .04f, .02f) },
			{ iris,  vec3( .105f, 1.78f, .272f), vec3(.05f, .04f, .02f) },
			{ skin,  vec3(0, 1.70f, .262f), vec3(.07f, .08f, .04f) },
			{ mouth, vec3(0, 1.62f, .255f), vec3(.12f, .025f, .03f) },
		};

		const uint count = (uint)(sizeof(parts) / sizeof(parts[0]));
		Instance inst[32] = {};
		for (uint i = 0; i < count; i++)
		{
			inst[i] = material(parts[i].color, 0.f, .84f);
			inst[i].model = base * glm::translate(mat4(1), parts[i].pos)
			              * glm::scale(mat4(1), parts[i].scale);
		}
		renderer->buffer(MODE_STATIC_UV).append_instances(arm_mesh, count, inst);
	}

	// E / number keys / F5. returns true when the inventory owns the input.
	bool inventory_input(GameWindow* window)
	{
		bool ours = window->focus_status == GAME;
		bool e_pressed = window->keys.E.is_pressed && !window->keys.E.was_pressed;
		if (has_inventory && e_pressed && (ours || inventory_open))
		{
			if (inventory_open)
			{
				// vanilla returns the crafting grid to the inventory on close
				for (uint i = 0; i < 4; i++)
					if (craft[i].block && craft[i].count && add_item(craft[i].block, craft[i].count))
						craft[i] = {};
				if (drag_stack.block && drag_stack.count &&
				    add_item(drag_stack.block, drag_stack.count))
					drag_stack = {};
				drag_source = -1;
			}
			inventory_open = !inventory_open;
			window->ui_lock = inventory_open;
			window->set_focus(inventory_open ? GUI : GAME);
		}
		if (inventory_open) return true;

		if (has_inventory)
		{
			if (window->keys.ONE.is_pressed   && !window->keys.ONE.was_pressed)   selected = 0;
			if (window->keys.TWO.is_pressed   && !window->keys.TWO.was_pressed)   selected = 1;
			if (window->keys.THREE.is_pressed && !window->keys.THREE.was_pressed) selected = 2;
			if (window->keys.FOUR.is_pressed  && !window->keys.FOUR.was_pressed)  selected = 3;
			if (window->keys.FIVE.is_pressed  && !window->keys.FIVE.was_pressed)  selected = 4;
			if (window->keys.SIX.is_pressed   && !window->keys.SIX.was_pressed)   selected = 5;
			if (window->keys.SEVEN.is_pressed && !window->keys.SEVEN.was_pressed) selected = 6;
			if (window->keys.EIGHT.is_pressed && !window->keys.EIGHT.was_pressed) selected = 7;
			if (window->keys.NINE.is_pressed  && !window->keys.NINE.was_pressed)  selected = 8;
			if (window->keys.F5.is_pressed    && !window->keys.F5.was_pressed)    third_person = !third_person;
		}
		return false;
	}

	// one frame : input -> movement -> camera
	void update(GameWindow* window, Physics* phys)
	{
		ImGuiIO& io = ImGui::GetIO();
		float dt = window->dtime;

		bool ours = window->focus_status == GAME;
		if (inventory_input(window)) return;
		arm_swing *= expf(-12.f * dt);

		// F : swap between the capsule and the free camera
		if (ours && !io.WantCaptureKeyboard &&
			window->keys.F.is_pressed && !window->keys.F.was_pressed)
			flycam = !flycam;

		if (flycam) return;

		vec2 look = vec2(0);
		if (ours && !io.WantCaptureMouse)
		{
			look = vec2((float)window->mouse.dx, (float)window->mouse.dy);
			camera.update_dir(look.x, look.y, dt);
		}

		move(window, phys); // every frame : the eyes ride the capsule
	}

	// capsule movement : velocity is set directly, gravity stays bullet's
	void move(GameWindow* window, Physics* phys)
	{
		if (!body) return;

		btRigidBody* b = phys->bodies[body].body;
		vec3 pos = phys->get_position(body);

		float half = PLAYER_HEIGHT * .5f + PLAYER_RADIUS;
		grounded = phys->raycast(pos, pos - vec3(0, half + .15f, 0)).hit;

		vec3 vel  = phys->get_velocity(body);
		vec3 flat = vec3(vel.x, 0, vel.z);

		// wish direction from the camera's yaw only : looking up never slows you
		vec3 fwd   = normalize(vec3(camera.front.x, 0, camera.front.z));
		vec3 right = normalize(vec3(camera.right.x, 0, camera.right.z));

		vec3 wish = vec3(0);
		if (window->keys.W.is_pressed) wish += fwd;
		if (window->keys.S.is_pressed) wish -= fwd;
		if (window->keys.D.is_pressed) wish += right;
		if (window->keys.A.is_pressed) wish -= right;
		if (length(wish) > 0) wish = normalize(wish);

		float speed = WALK_SPEED * (window->keys.SHIFT.is_pressed ? SPRINT_SCALE : 1.f);
		float accel = grounded ? 12.f : 2.5f; // weak air control, on purpose
		flat += (wish * speed - flat) * (1.f - expf(-accel * window->dtime));

		if (grounded && window->keys.SPACE.is_pressed && !window->keys.SPACE.was_pressed)
			vel.y = JUMP_SPEED;

		b->setLinearVelocity(btVector3(flat.x, vel.y, flat.z));
		b->activate(true); // the player is always in play

		// eyes : capsule center, an eye height up, bobbing with the walk
		float walk = length(flat);
		bob += window->dtime * (1.f + walk);
		float amp = grounded ? walk / WALK_SPEED : 0;
		camera.position = pos + vec3(cosf(bob * 2.f) * .02f * amp,
		                             EYE_HEIGHT + sinf(bob * 4.f) * .022f * amp, 0);
	}
};
