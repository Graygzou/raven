#include <string>
#include <locale>
#include "Raven_Bot.h"
#include "misc/Cgdi.h"
#include "misc/utils.h"
#include "2D/Transformations.h"
#include "2D/Geometry.h"
#include "lua/Raven_Scriptor.h"
#include "Raven_Game.h"
#include "navigation/Raven_PathPlanner.h"
#include "Raven_SteeringBehaviors.h"
#include "Raven_UserOptions.h"
#include "time/Regulator.h"
#include "Raven_WeaponSystem.h"
#include "Raven_SensoryMemory.h"

#include "Messaging/Telegram.h"
#include "Raven_Messages.h"
#include "Messaging/MessageDispatcher.h"

#include "goals/Raven_Goal_Types.h"
#include "goals/Goal_Think.h"
#include "Raven_Bot.h"

#include "Debug/DebugConsole.h"


//-------------------------- ctor ---------------------------------------------
Raven_Bot::Raven_Bot(Raven_Game* world,Vector2D pos):

  MovingEntity(pos,
               script->GetDouble("Bot_Scale"),
               Vector2D(0,0),
               script->GetDouble("Bot_MaxSpeed"),
               Vector2D(1,0),
               script->GetDouble("Bot_Mass"),
               Vector2D(script->GetDouble("Bot_Scale"),script->GetDouble("Bot_Scale")),
               script->GetDouble("Bot_MaxHeadTurnRate"),
               script->GetDouble("Bot_MaxForce")),
                 
                 m_iMaxHealth(script->GetInt("Bot_MaxHealth")),
                 m_iHealth(script->GetInt("Bot_MaxHealth")),
                 m_pPathPlanner(NULL),
                 m_pSteering(NULL),
                 m_pWorld(world),
                 m_pBrain(NULL),
                 m_iNumUpdatesHitPersistant((int)(FrameRate * script->GetDouble("HitFlashTime"))),
                 m_bHit(false),
                 m_iScore(0),
                 m_Status(spawning),
                 m_bPossessed(false),
				 m_bLearner(false),
				 current_team(0),
                 m_dFieldOfView(DegsToRads(script->GetDouble("Bot_FOV")))
           
{
  SetEntityType(type_bot);

  SetUpVertexBuffer();
  
  //a bot starts off facing in the direction it is heading
  m_vFacing = m_vHeading;

  //create the navigation module
  m_pPathPlanner = new Raven_PathPlanner(this);

  //create the steering behavior class
  m_pSteering = new Raven_Steering(world, this);

  //create the regulators
  m_pWeaponSelectionRegulator = new Regulator(script->GetDouble("Bot_WeaponSelectionFrequency"));
  m_pGoalArbitrationRegulator =  new Regulator(script->GetDouble("Bot_GoalAppraisalUpdateFreq"));
  m_pTargetSelectionRegulator = new Regulator(script->GetDouble("Bot_TargetingUpdateFreq"));
  m_pTriggerTestRegulator = new Regulator(script->GetDouble("Bot_TriggerUpdateFreq"));
  m_pVisionUpdateRegulator = new Regulator(script->GetDouble("Bot_VisionUpdateFreq"));

  //create the goal queue
  m_pBrain = new Goal_Think(this);

  //create the targeting system
  m_pTargSys = new Raven_TargetingSystem(this);

  m_pWeaponSys = new Raven_WeaponSystem(this,
                                        script->GetDouble("Bot_ReactionTime"),
                                        script->GetDouble("Bot_AimAccuracy"),
                                        script->GetDouble("Bot_AimPersistance"));

  m_pSensoryMem = new Raven_SensoryMemory(this, script->GetDouble("Bot_MemorySpan"));
}

//-------------------------------- dtor ---------------------------------------
//-----------------------------------------------------------------------------
Raven_Bot::~Raven_Bot()
{
  debug_con << "deleting raven bot (id = " << ID() << ")" << "";
  
  delete m_pBrain;
  delete m_pPathPlanner;
  delete m_pSteering;
  delete m_pWeaponSelectionRegulator;
  delete m_pTargSys;
  delete m_pGoalArbitrationRegulator;
  delete m_pTargetSelectionRegulator;
  delete m_pTriggerTestRegulator;
  delete m_pVisionUpdateRegulator;
  delete m_pWeaponSys;
  delete m_pSensoryMem;
}

//-------------------------- SetBrainBehavior ---------------------------------
//  give a personnality to the bot by giving customs weights to his goals
//  0 : Standard.
//  1 : Burnhead behavior (like to fight and explore)
//  2 : Coward  (like take health)
//  3 : Weapons Collector (like to collect all kind of weapons)
//-----------------------------------------------------------------------------
void Raven_Bot::SetBrainBehavior(int behavior) {
	switch (behavior) {
		case 1:
			//replace the old brain by a more personnal one
			m_pBrain = new Goal_Think(this, 0.5, 0.5, 0.5, 0.5, 0.5, 1.5, 1.5);
			break;
		case 2:
			//replace the old brain by a more personnal one
			m_pBrain = new Goal_Think(this, 1.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5);
			break;
		case 3:
			//replace the old brain by a more personnal one
			m_pBrain = new Goal_Think(this, 0.5, 1.5, 1.5, 1.5, 1.5, 0.5, 0.5);
			break;
		default:
			// nothing
			break;
	}
	// Assigne the behavior of the bot to render them better
	m_bBehavior = behavior;

}

//------------------------------- Spawn ---------------------------------------
//
//  spawns the bot at the given position
//-----------------------------------------------------------------------------
void Raven_Bot::Spawn(Vector2D pos)
{
    SetAlive();
    m_pBrain->RemoveAllSubgoals();
	//if the bot is under AI control but not scripted
	if (!isPossessed())
		m_pTargSys->ClearTarget();
    SetPos(pos);
    m_pWeaponSys->Initialize();
    RestoreHealthToMaximum();
}

//-------------------------------- Update -------------------------------------
//
void Raven_Bot::Update()
{
  //process the currently active goal. Note this is required even if the bot
  //is under user control. This is because a goal is created whenever a user 
  //clicks on an area of the map that necessitates a path planning request.
  m_pBrain->Process();
  
  //Calculate the steering force and update the bot's velocity and position
  UpdateMovement();

  //if the bot is under AI control but not scripted
  if (!isPossessed())
  {           
    //examine all the opponents in the bots sensory memory and select one
    //to be the current target
    if (m_pTargetSelectionRegulator->isReady())
    { 
		if (this->HasTeam()) { // if has a team
			if (current_team->GetTarget() == 0) { // if no current target
				m_pTargSys->Update();
				if (m_pTargSys->GetTarget() != 0) { //if found a target
					current_team->UpdateNewTarget(m_pTargSys->GetTarget(), ID()); // we give new taret to other member of the team
				}
			}
			else {
				if (m_pTargSys->GetTarget() == 0) {//if his team has a target but he dont we give to him (case if bot respawn)
					m_pTargSys->SetTarget(current_team->GetTarget());
				}
			}
		}
		else {
			m_pTargSys->Update();
		}
		
    }

    //appraise and arbitrate between all possible high level goals
    if (m_pGoalArbitrationRegulator->isReady())
    {
       m_pBrain->Arbitrate(); 
    }

    //update the sensory memory with any visual stimulus
    if (m_pVisionUpdateRegulator->isReady())
    {
      m_pSensoryMem->UpdateVision();
    }
  
    //select the appropriate weapon to use from the weapons currently in
    //the inventory
    if (m_pWeaponSelectionRegulator->isReady())
    {       
      m_pWeaponSys->SelectWeapon();       
    }

	double angle = m_pWeaponSys->GetBotAim();

    //this method aims the bot's current weapon at the current target
    //and takes a shot if a shot is possible
    m_pWeaponSys->TakeAimAndShoot(angle);
  }
  else
  {
	  //update the sensory memory with any visual stimulus
	  if (m_pVisionUpdateRegulator->isReady())
	  {
		  m_pSensoryMem->UpdateVision();
	  }
  }
}


//------------------------- UpdateMovement ------------------------------------
//
//  this method is called from the update method. It calculates and applies
//  the steering force for this time-step.
//-----------------------------------------------------------------------------
void Raven_Bot::UpdateMovement()
{
  //calculate the combined steering force
  Vector2D force = m_pSteering->Calculate();

  //if no steering force is produced decelerate the player by applying a
  //braking force
  if (m_pSteering->Force().isZero())
  {
    const double BrakingRate = 0.8; 

    m_vVelocity = m_vVelocity * BrakingRate;                                     
  }

  //calculate the acceleration
  Vector2D accel = force / m_dMass;

  //update the velocity
  m_vVelocity += accel;

  //make sure vehicle does not exceed maximum velocity
  m_vVelocity.Truncate(m_dMaxSpeed);

  //update the position
  m_vPosition += m_vVelocity;

  //if the vehicle has a non zero velocity the heading and side vectors must 
  //be updated
  if (!m_vVelocity.isZero())
  {    
    m_vHeading = Vec2DNormalize(m_vVelocity);

    m_vSide = m_vHeading.Perp();
  }
}
//---------------------------- isReadyForTriggerUpdate ------------------------
//
//  returns true if the bot is ready to be tested against the world triggers
//-----------------------------------------------------------------------------
bool Raven_Bot::isReadyForTriggerUpdate()const
{
  return m_pTriggerTestRegulator->isReady();
}

// Used to make an uppercase word
std::string Convert(std::string& str)
{
	std::locale settings;
	std::string converted;

	for (short i = 0; i < str.size(); ++i)
		converted += (std::toupper(str[i], settings));

	return converted;
}

//--------------------------- HandleMessage -----------------------------------
//-----------------------------------------------------------------------------
bool Raven_Bot::HandleMessage(const Telegram& msg)
{
  //first see if the current goal accepts the message
  if (GetBrain()->HandleMessage(msg)) return true;
 
  //handle any messages not handles by the goals
  switch(msg.Msg)
  {
  case Msg_TakeThatMF:

    //just return if already dead or spawning
    if (isDead() || isSpawning()) return true;

    //the extra info field of the telegram carries the amount of damage
    ReduceHealth(DereferenceToType<int>(msg.ExtraInfo));

    //if this bot is now dead let the shooter know
    if (isDead())
    {
		debug_con << "Bot is Dead" << "";
		DropWeapon();
      Dispatcher->DispatchMsg(SEND_MSG_IMMEDIATELY,
                              ID(),
                              msg.Sender,
                              Msg_YouGotMeYouSOB,
                              NO_ADDITIONAL_INFO);
    }

    return true;

  case Msg_YouGotMeYouSOB:
    
    IncrementScore();

    //the bot this bot has just killed should be removed as the target
	if (this->HasTeam() ) {
		// Update the score of the current bot
		//"IDC_SCORE" + current_team->GetName() + 'J' + ttos(m_bNumber);
		//std::string id = "IDC_SCORE_" + Convert(current_team->GetName()) + 'J' + ttos(m_bNumber));
		//debug_con << ("IDC_SCORE_" + Convert(current_team->GetName()) + 'J' + ttos(m_bNumber)) << "";
		//SetDlgItemText(IDC_STATIC, "New text here.");
		// Clear the target
		current_team->ClearTarget(ID());
	}
	else {
		m_pTargSys->ClearTarget();
	}



    return true;

  case Msg_GunshotSound:

    //add the source of this sound to the bot's percepts
    GetSensoryMem()->UpdateWithSoundSource((Raven_Bot*)msg.ExtraInfo);

    return true;

  case Msg_UserHasRemovedBot:
    {

      Raven_Bot* pRemovedBot = (Raven_Bot*)msg.ExtraInfo;

      GetSensoryMem()->RemoveBotFromMemory(pRemovedBot);

      //if the removed bot is the target, make sure the target is cleared
      if (pRemovedBot == GetTargetSys()->GetTarget())
      {
        GetTargetSys()->ClearTarget();
      }

      return true;
    }

  case Msg_UpdatingTarget : 
  {
	  m_pTargSys->SetTarget(current_team->GetTarget()); // modify target
	  return true;
  }

  case Msg_TargetKilled: {
	  m_pTargSys->ClearTarget(); //clear target
	  return true;
  }


  default: return false;
  }
}

//------------------ RotateFacingTowardPosition -------------------------------
//
//  given a target position, this method rotates the bot's facing vector
//  by an amount not greater than m_dMaxTurnRate until it
//  directly faces the target.
//
//  returns true when the heading is facing in the desired direction
//----------------------------------------------------------------------------
bool Raven_Bot::RotateFacingTowardPosition(Vector2D target)
{
  Vector2D toTarget = Vec2DNormalize(target - m_vPosition);

  double dot = m_vFacing.Dot(toTarget);

  //clamp to rectify any rounding errors
  Clamp(dot, -1, 1);

  //determine the angle between the heading vector and the target
  double angle = acos(dot);

  //return true if the bot's facing is within WeaponAimTolerance degs of
  //facing the target
  const double WeaponAimTolerance = 0.01; //2 degs approx

  if (angle < WeaponAimTolerance)
  {
    m_vFacing = toTarget;
    return true;
  }

  //clamp the amount to turn to the max turn rate
  if (angle > m_dMaxTurnRate) angle = m_dMaxTurnRate;
  
  //The next few lines use a rotation matrix to rotate the player's facing
  //vector accordingly
  C2DMatrix RotationMatrix;
  
  //notice how the direction of rotation has to be determined when creating
  //the rotation matrix
  RotationMatrix.Rotate(angle * m_vFacing.Sign(toTarget));	
  RotationMatrix.TransformVector2Ds(m_vFacing);

  return false;
}




//--------------------------------- ReduceHealth ----------------------------
void Raven_Bot::ReduceHealth(unsigned int val)
{
  m_iHealth -= val;

  if (m_iHealth <= 0)
  {
    SetDead();
  }

  m_bHit = true;

  m_iNumUpdatesHitPersistant = (int)(FrameRate * script->GetDouble("HitFlashTime"));
}

//--------------------------- Possess -----------------------------------------
//
//  this is called to allow a human player to control the bot
//-----------------------------------------------------------------------------
void Raven_Bot::TakePossession()
{
  if ( !(isSpawning() || isDead()))
  {
    m_bPossessed = true;

    debug_con << "Player Possesses bot " << this->ID() << "";
  }
}
//------------------------------- Exorcise ------------------------------------
//
//  called when a human is exorcised from this bot and the AI takes control
//-----------------------------------------------------------------------------
void Raven_Bot::Exorcise()
{
  m_bPossessed = false;

  //when the player is exorcised then the bot should resume normal service
  m_pBrain->AddGoal_Explore();
  
  debug_con << "Player is exorcised from bot " << this->ID() << "";
}

//--------------------------- Learning -----------------------------------------
//
//  this is called to allow a bot to learn how to shoot
//-----------------------------------------------------------------------------
void Raven_Bot::BecomeLearner()
{
	m_bLearner = true;
}


//----------------------- ChangeWeapon ----------------------------------------
void Raven_Bot::ChangeWeapon(unsigned int type)
{
  m_pWeaponSys->ChangeWeapon(type);
}
  

//---------------------------- FireWeapon -------------------------------------
//
//  fires the current weapon at the given position
//-----------------------------------------------------------------------------
void Raven_Bot::FireWeapon(Vector2D pos)
{
	// Fire with the position
	m_pWeaponSys->ShootAt(pos);
}

//----------------- CalculateExpectedTimeToReachPosition ----------------------
//
//  returns a value indicating the time in seconds it will take the bot
//  to reach the given position at its current speed.
//-----------------------------------------------------------------------------
double Raven_Bot::CalculateTimeToReachPosition(Vector2D pos)const
{
  return Vec2DDistance(Pos(), pos) / (MaxSpeed() * FrameRate);
}

//------------------------ isAtPosition ---------------------------------------
//
//  returns true if the bot is close to the given position
//-----------------------------------------------------------------------------
bool Raven_Bot::isAtPosition(Vector2D pos)const
{
  const static double tolerance = 10.0;
  
  return Vec2DDistanceSq(Pos(), pos) < tolerance * tolerance;
}

//------------------------- hasLOSt0 ------------------------------------------
//
//  returns true if the bot has line of sight to the given position.
//-----------------------------------------------------------------------------
bool Raven_Bot::hasLOSto(Vector2D pos)const
{
  return m_pWorld->isLOSOkay(Pos(), pos);
}

//returns true if this bot can move directly to the given position
//without bumping into any walls
bool Raven_Bot::canWalkTo(Vector2D pos)const
{
  return !m_pWorld->isPathObstructed(Pos(), pos, BRadius());
}

//similar to above. Returns true if the bot can move between the two
//given positions without bumping into any walls
bool Raven_Bot::canWalkBetween(Vector2D from, Vector2D to)const
{
 return !m_pWorld->isPathObstructed(from, to, BRadius());
}

//--------------------------- canStep Methods ---------------------------------
//
//  returns true if there is space enough to step in the indicated direction
//  If true PositionOfStep will be assigned the offset position
//---------------------------TakePossession--------------------------------------------------
bool Raven_Bot::canStepLeft(Vector2D& PositionOfStep)const
{
  static const double StepDistance = BRadius() * 2;

  PositionOfStep = Pos() - Facing().Perp() * StepDistance - Facing().Perp() * BRadius();

  return canWalkTo(PositionOfStep);
}

bool Raven_Bot::canStepRight(Vector2D& PositionOfStep)const
{
  static const double StepDistance = BRadius() * 2;

  PositionOfStep = Pos() + Facing().Perp() * StepDistance + Facing().Perp() * BRadius();

  return canWalkTo(PositionOfStep);
}

bool Raven_Bot::canStepForward(Vector2D& PositionOfStep)const
{
  static const double StepDistance = BRadius() * 2;

  PositionOfStep = Pos() + Facing() * StepDistance + Facing() * BRadius();

  return canWalkTo(PositionOfStep);
}

bool Raven_Bot::canStepBackward(Vector2D& PositionOfStep)const
{
  static const double StepDistance = BRadius() * 2;

  PositionOfStep = Pos() - Facing() * StepDistance - Facing() * BRadius();

  return canWalkTo(PositionOfStep);
}

//--------------------------- Render -------------------------------------
//
//------------------------------------------------------------------------
void Raven_Bot::Render()                                         
{
  //when a bot is hit by a projectile this value is set to a constant user
  //defined value which dictates how long the bot should have a thick red
  //circle drawn around it (to indicate it's been hit) The circle is drawn
  //as long as this value is positive. (see Render)
  m_iNumUpdatesHitPersistant--;


  if (isDead() || isSpawning()) return;
  
  if (this->HasTeam()) {
	  if (this->GetTeamName() == "Alpha") { //Team Alpha rouge
		  gdi->BluePen();
	  }
	  if (this->GetTeamName() == "Beta") { //Team beta bleue
		  gdi->RedPen();
	  }
  } else {
	  if (m_bLearner) {
		  gdi->BlackPen(); // learning = black
	  } else {
		  switch (m_bBehavior) {
			case 0:
				gdi->GreenPen(); // Regular bot
				break;
			case 1:
				gdi->RedPen(); // Burnhead bot
				break;
			case 2:
				gdi->BluePen(); // Coward bot
				break;
			case 3:
				gdi->OrangePen(); // Weapons Collector  bot
				break;
		  }
		  
	  }
	  
  }

  
  m_vecBotVBTrans = WorldTransform(m_vecBotVB,
                                   Pos(),
                                   Facing(),
                                   Facing().Perp(),
                                   Scale());

  gdi->ClosedShape(m_vecBotVBTrans);
  
  //draw the head
  gdi->BrownBrush();
  gdi->Circle(Pos(), 6.0 * Scale().x);


  //render the bot's weapon
  m_pWeaponSys->RenderCurrentWeapon();

  //render a thick red circle if the bot gets hit by a weapon
  if (m_bHit)
  {
    gdi->ThickRedPen();
    gdi->HollowBrush();
    gdi->Circle(m_vPosition, BRadius()+1);

    if (m_iNumUpdatesHitPersistant <= 0)
    {
      m_bHit = false;
    }
  }

  gdi->TransparentText();
  if (this->HasTeam()) {
	  if (this->GetTeamName() == "Alpha")
		  gdi->TextColor(0, 0, 255);
	  if (this->GetTeamName() == "Beta")
		  gdi->TextColor(255, 0, 0);
  } else {
	  if (m_bLearner)
		  gdi->TextColor(0, 0, 0);
	  else
		  gdi->TextColor(0, 255, 0);
  }
  
  if (UserOptions->m_bShowBotIDs)
  {
    gdi->TextAtPos(Pos().x -10, Pos().y-20, ttos(ID()));
  }

  if (UserOptions->m_bShowBotHealth)
  {
    gdi->TextAtPos(Pos().x-40, Pos().y-5, "H:"+ ttos(Health()));
  }

  if (UserOptions->m_bShowScore)
  {
    gdi->TextAtPos(Pos().x-40, Pos().y+10, "Scr:"+ ttos(Score()));
  }    
}

//------------------------- SetUpVertexBuffer ---------------------------------
//-----------------------------------------------------------------------------
void Raven_Bot::SetUpVertexBuffer()
{
  //setup the vertex buffers and calculate the bounding radius
  const int NumBotVerts = 4;
  const Vector2D bot[NumBotVerts] = {Vector2D(-3, 8),
                                     Vector2D(3,10),
                                     Vector2D(3,-10),
                                     Vector2D(-3,-8)};

  m_dBoundingRadius = 0.0;
  double scale = script->GetDouble("Bot_Scale");
  
  for (int vtx=0; vtx<NumBotVerts; ++vtx)
  {
    m_vecBotVB.push_back(bot[vtx]);

    //set the bounding radius to the length of the 
    //greatest extent
    if (abs(bot[vtx].x)*scale > m_dBoundingRadius)
    {
      m_dBoundingRadius = abs(bot[vtx].x*scale);
    }

    if (abs(bot[vtx].y)*scale > m_dBoundingRadius)
    {
      m_dBoundingRadius = abs(bot[vtx].y)*scale;
    }
  }
}



void Raven_Bot::RestoreHealthToMaximum(){m_iHealth = m_iMaxHealth;}

void Raven_Bot::IncreaseHealth(unsigned int val)
{
  m_iHealth+=val; 
  Clamp(m_iHealth, 0, m_iMaxHealth);
}


void Raven_Bot::DropWeapon() {
	if (this->HasTeam() && this->team_type == 0) { //TeamSimple
		for (unsigned int weapon_type = 6; weapon_type < 11; weapon_type++) {
			switch (weapon_type) {
			case type_shotgun: {
				Raven_Weapon* current_weapon = m_pWeaponSys->GetWeaponFromInventory(type_shotgun);
				if (current_weapon != NULL) {
					//shotgun mais 8 sur la map
					debug_con << "Creating a Shootgun" << "";
					/*std::ifstream test;
					m_pWorld->GetMap()->lastgraphnodeindex += 1;
					int index = m_pWorld->GetMap()->lastgraphnodeindex;
					//test = (std::ifstream) (std::to_string(current_team->GetLootingLocation().x) + " " + std::to_string(current_team->GetLootingLocation().y)+" 7 "+ std::to_string(index));
					test = (std::ifstream) ("401  360 60 7  340");
					m_pWorld->GetMap()->AddWeapon_Giver_bis(8, test, false);*/
					m_pWorld->GetMap()->AddDroppedWeaponTrigger(current_team->GetLootingLocation(), current_weapon->GetType(), current_weapon->NumRoundsRemaining(), m_pWorld);
				}
				break;
			}
			case type_rail_gun: {//railgun 6 sur la map 
				Raven_Weapon* current_weapon = m_pWeaponSys->GetWeaponFromInventory(type_rail_gun);
				if (current_weapon != NULL) {
					debug_con << "Creating a Railgun" << "";
					/*std::ifstream test;
					m_pWorld->GetMap()->lastgraphnodeindex += 1;
					int index = m_pWorld->GetMap()->lastgraphnodeindex;
					//test = (std::ifstream) (std::to_string(current_team->GetLootingLocation().x) + " " + std::to_string(current_team->GetLootingLocation().y) + " 7 " + std::to_string(index));
					test = (std::ifstream) ("401  360 60 7  340");
					m_pWorld->GetMap()->AddWeapon_Giver_bis(6, test, false);*/
					m_pWorld->GetMap()->AddDroppedWeaponTrigger(current_team->GetLootingLocation(), current_weapon->GetType(), current_weapon->NumRoundsRemaining(), m_pWorld);
				}
				break;
			}
			case type_rocket_launcher: {//rocket_launcher 7 sur la map 
				Raven_Weapon* current_weapon = m_pWeaponSys->GetWeaponFromInventory(type_rocket_launcher);
				if (current_weapon != NULL) {
					debug_con << "Creating a Rocket_Launcher" << "";
					/*std::ifstream test;
					m_pWorld->GetMap()->lastgraphnodeindex += 1;
					int index = m_pWorld->GetMap()->lastgraphnodeindex;
					//test = (std::ifstream) (std::to_string(current_team->GetLootingLocation().x) + " " + std::to_string(current_team->GetLootingLocation().y) + " 7 " + std::to_string(index));
					test = (std::ifstream) ("401  360 60 7  340");
					m_pWorld->GetMap()->AddWeapon_Giver_bis(7, test, false);*/
					m_pWorld->GetMap()->AddDroppedWeaponTrigger(current_team->GetLootingLocation(), current_weapon->GetType(), current_weapon->NumRoundsRemaining(), m_pWorld);
				}
				break;
			}
			case type_grenade: {//grenade 10 sur a map 
				Raven_Weapon* current_weapon = m_pWeaponSys->GetWeaponFromInventory(type_grenade);
				if (current_weapon != NULL) {
					/*debug_con << "Creating a grenade" << "";
					std::ifstream test;
					m_pWorld->GetMap()->lastgraphnodeindex += 1;
					int index = m_pWorld->GetMap()->lastgraphnodeindex;
					//test = (std::ifstream) (std::to_string(current_team->GetLootingLocation().x) + " " + std::to_string(current_team->GetLootingLocation().y) + " 7 " + std::to_string(index));
					test = (std::ifstream) ("401  360 60 7  340");
					m_pWorld->GetMap()->AddWeapon_Giver_bis(10, test, false);*/
					m_pWorld->GetMap()->AddDroppedWeaponTrigger(current_team->GetLootingLocation(), current_weapon->GetType(), current_weapon->NumRoundsRemaining(), m_pWorld);
				}
				break;
			}

			}
		}

	}
}
